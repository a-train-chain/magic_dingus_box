"""Pure recovery policy. Durations are monotonic; history belongs to runtime.

Unknown/stale observations never prove failure or readiness. API errors alone
cannot rotate a working tunnel, and qBit failures only request a port sync.

Generation = container identity + tun0 instance. Only a new CONTAINER start
restarts the failure clocks and grants a fresh candidate grace. Gluetun
replaces tun0 inside the same container on its own (it restart-loops a dead
endpoint every few seconds); those replacements keep the clocks running, and a
short unknown gap (<= UNKNOWN_TOLERANCE_S) keeps established loss evidence
without counting as failure itself.
"""
import copy
import math

UNKNOWN_TOLERANCE_S = 30
# A tun0 seen this recently in the same container means a missing tun0 is one
# more restart of Gluetun's own retry loop, not a dead control path.
TUNNEL_GAP_S = 30
CONTAINER_KEYS = ('container_id', 'pid', 'started_at')


def same_container(a, b):
    return (isinstance(a, dict) and isinstance(b, dict)
            and all(a.get(k) is not None and a.get(k) == b.get(k) for k in CONTAINER_KEYS))


def finite(value):
    try:
        return type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        return False


def decide(observation, state, config, now_mono, now_wall):
    o, now = observation, now_mono
    s = copy.deepcopy(state) if isinstance(state, dict) and state.get('version') == 2 else {}
    # Malformed timers and backwards monotonic clocks discard in-process state.
    timers = ('started', 'last_tick', 'port_lost', 'traffic_lost', 'control_lost', 'candidate_until', 'backoff_until', 'last_restart',
              'traffic_unknown', 'port_unknown', 'tunnel_seen')
    if any(k in s and (not finite(s[k]) or s[k] > now and k not in ('candidate_until', 'backoff_until')) for k in timers):
        s = {}
    if not isinstance(s.get('losses', []), list):
        s = {}
    s.setdefault('started', now)
    s.update(version=2, last_tick=now)
    s['losses'] = [t for t in s.get('losses', [])[-256:] if finite(t) and 0 <= now-t < 600]

    def answer(action, reason, **extra):
        return dict(action=action, reason=reason, next_state=s, next_due=now+10, **extra)

    if not (config.get('provider') == 'protonvpn' and config.get('type') == 'wireguard' and config.get('forwarding')):
        return answer('none', 'Unsupported configuration')
    if o.get('allowed') is not True:
        return answer('none', 'Stopped or maintenance')
    if o.get('running') is None:
        return answer('none', 'Docker unavailable')
    if o.get('busy'):
        return answer('none', 'Recovery in progress')

    def restart(reason):
        if 'last_restart' in s and now-s['last_restart'] < 600:
            return answer('none', 'Restart rate limited')
        s.update(last_restart=now, candidate_until=now+120)
        return answer('restart_container', reason)

    if not o.get('running'):
        if o.get('affirmative_failure') is True:
            return restart('Recovering failed container')
        return answer('none', 'Stopped; operator start required')

    generation = o.get('generation')
    if generation is not None and generation != s.get('generation'):
        had_generation = 'generation' in s
        tunnel_only = same_container(s.get('generation'), generation)
        s['generation'] = generation
        if not tunnel_only:
            s.update(started=now, had_port=False, port_was_present=False)
            for k in ('port_lost', 'traffic_lost', 'control_lost', 'traffic_unknown', 'port_unknown', 'tunnel_seen'):
                s.pop(k, None)
            if had_generation:
                s['candidate_until'] = now+120
    if generation is not None:
        s['tunnel_seen'] = now
    traffic = o.get('traffic')
    if not finite(o.get('traffic_at')) or not 0 <= now-o['traffic_at'] <= 40:
        traffic = None
    if o.get('traffic_generation', generation) != generation:
        traffic = None
    port = o.get('port')
    if (generation is None or not finite(o.get('port_at')) or not 0 <= now-o['port_at'] <= 15
            or o.get('port_generation', generation) != generation or type(port) is not int or not 0 <= port <= 65535):
        port = None
    if port is not None:
        s.pop('port_unknown', None)
        if port > 0:
            s.update(had_port=True, port_was_present=True)
            s.pop('port_lost', None)
        else:
            if s.get('port_was_present'):
                s['losses'].append(now)
            s['port_was_present'] = False
            s.setdefault('port_lost', now)
    else:
        s.setdefault('port_unknown', now)
        if now-s['port_unknown'] > UNKNOWN_TOLERANCE_S:
            s.pop('port_lost', None)
    if o.get('control_available') is False:
        s.setdefault('control_lost', now)
    else:
        s.pop('control_lost', None)
    if traffic is False:
        s.pop('traffic_unknown', None)
        s.setdefault('traffic_lost', now)
    elif traffic is True:
        s.pop('traffic_unknown', None)
        s.pop('traffic_lost', None)
    else:
        s.setdefault('traffic_unknown', now)
        if now-s['traffic_unknown'] > UNKNOWN_TOLERANCE_S:
            s.pop('traffic_lost', None)

    if o.get('wan') is False and traffic is not True:
        return answer('none', 'Waiting for internet')
    # qBit cannot synchronize through an orphaned namespace (or while absent).
    # Repair proven linkage first, without turning a qBit-only fault into a
    # restart. A bounded repair retry must not publish Ready in the meantime.
    if traffic is True and o.get('needs_relink'):
        return answer('relink_dependents', 'Relinking services')
    if traffic is True and o.get('relink_waiting'):
        return answer('none', 'Relinking services')
    if traffic is True and o.get('needs_converge'):
        return answer('converge_dependents', 'Restoring services')
    if traffic is True and port and o.get('selection_verified', True):
        # A healthy fallback is sticky; historical penalties remain in runtime.
        s.pop('candidate_until', None)
        s.pop('backoff_until', None)
        s['backoff_level'] = 0
        if not o.get('qbit_synced'):
            return answer('sync_port', 'Synchronizing port')
        if o.get('namespaces_match') is not True or o.get('dependents_ok') is not True:
            return answer('none', 'Verifying services')
        return answer('none', 'Ready')

    if now < s.get('candidate_until', 0):
        return answer('none', 'Waiting for candidate')
    new_round = False
    if 'backoff_until' in s:
        if now < s['backoff_until']:
            return answer('none', 'Recovery backoff')
        s.pop('backoff_until')
        new_round = True
    control_down = (o.get('control_available') is False
                    and now-s.get('control_lost', now) >= (90 if s.get('had_port') else 120))
    failure = (traffic is False and now-s.get('traffic_lost', now) >= 60
               or port == 0 and ((not s.get('had_port') and now-s['started'] >= 120)
                                  or s.get('had_port') and now-s.get('port_lost', now) >= 90
                                  or len(s['losses']) >= 3)
               or control_down
               or o.get('selection_failed') is True)
    if not failure:
        return answer('none', 'Recovering port forwarding' if traffic is True and port == 0 else 'Verifying VPN')
    catalog_or_api = o.get('api_failed') or o.get('catalog_unattested') or o.get('catalog_needs_load')
    if (generation is None and not catalog_or_api and same_container(o, s.get('generation'))
            and finite(s.get('tunnel_seen')) and now-s['tunnel_seen'] <= TUNNEL_GAP_S):
        # Mid-restart gap (control reads need tun0 too): rotate on the next
        # tick that sees a tunnel rather than spend the one container restart.
        return answer('none', 'Recovering VPN')
    # An unconfirmed control read failure (e.g. tun0 replaced mid-read) does
    # not justify the restart; a rotation PUT that then fails sets api_failed.
    if generation is None or control_down or catalog_or_api:
        return restart('Loading catalog or recovering control API')
    if not o.get('candidate_available', True):
        level = s.get('backoff_level', 0)
        level = level if type(level) is int and 0 <= level <= 2 else 0
        s.update(backoff_until=now+(120, 300, 600)[level], backoff_level=min(2, level+1))
        return answer('none', 'Recovery backoff')
    s['candidate_until'] = now+120
    return answer('rotate_server', 'Recovering port forwarding' if traffic is True else 'Recovering VPN', new_round=new_round)
