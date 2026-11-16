"""Validated static Proton data. Disk identity never attests Gluetun memory.

The runtime owns a loaded Catalog snapshot and the container generation that
attests it. Refreshing this store changes only the downloaded snapshot. New
hostnames must not be tried until a necessary, bounded restart loads that file.
"""
import base64
import copy
from dataclasses import dataclass
import hashlib
import http.client
import ipaddress
import json
import math
import os
from pathlib import Path
import random
import re
import stat
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlsplit

from .common import atomic_json, bounded_lock, load_config

CATALOG_URL = 'https://raw.githubusercontent.com/qdm12/gluetun-servers/main/pkg/servers/protonvpn.json'
MAX_BYTES = 8*1024*1024
HISTORY_LIMIT = 256
# A failed endpoint is not selected again for this long.
EXCLUSION_S = 1800
# A server that failed this many times, each within 24 h of the one before,
# goes to the back of its pool (never out of it). The stored count is capped.
REPEAT_FAILURES = 2
REPEAT_COUNT_CAP = 16


@dataclass(frozen=True)
class Candidate:
    hostname: str
    country: str
    endpoint_ip: str
    public_key: str


@dataclass(frozen=True)
class Catalog:
    provider: dict
    identity: str
    timestamp: int


@dataclass(frozen=True)
class FetchResponse:
    status: int
    body: bytes
    headers: dict


def _finite(value):
    """Reject nonnumeric JSON and integers too large for finite time arithmetic."""
    if type(value) not in (int, float):
        return False
    try:
        return math.isfinite(value)
    except OverflowError:
        return False


def _json(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('duplicate JSON field')
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=pairs,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError('invalid JSON number')))


def validate_catalog(provider, now):
    """Accept pinned schema 4 only; reject a malformed endpoint, not just skip it."""
    if not _finite(now) or not isinstance(provider, dict) or type(provider.get('version')) is not int or provider['version'] != 4:
        raise ValueError('incompatible catalog')
    timestamp = provider.get('timestamp')
    servers = provider.get('servers')
    if type(timestamp) is not int or timestamp <= 0 or timestamp > int(now):
        raise ValueError('invalid catalog timestamp')
    if not isinstance(servers, list) or not servers or len(servers) > 50000:
        raise ValueError('empty or oversized catalog')
    for server in servers:
        if not isinstance(server, dict) or server.get('vpn') not in ('wireguard', 'openvpn'):
            raise ValueError('invalid server protocol')
        hostname, country, ips = server.get('hostname'), server.get('country'), server.get('ips')
        if not isinstance(hostname, str) or not re.fullmatch(r'(?=.{1,253}$)[a-z0-9]+(?:[-.][a-z0-9]+)*\.protonvpn\.net', hostname):
            raise ValueError('invalid server hostname')
        if not isinstance(country, str) or not re.fullmatch(r"[A-Za-z][A-Za-z ,.'-]{0,79}", country):
            raise ValueError('invalid server country')
        if not isinstance(ips, list) or not 1 <= len(ips) <= 16:
            raise ValueError('invalid endpoint addresses')
        for ip in ips:
            if not isinstance(ip, str) or '%' in ip:
                raise ValueError('invalid endpoint address')
            try:
                if not ipaddress.ip_address(ip).is_global:
                    raise ValueError('non-public endpoint')
            except ValueError:
                raise ValueError('invalid endpoint address') from None
        for flag in ('port_forward', 'secure_core', 'free', 'tcp', 'udp', 'stream'):
            if flag in server and type(server[flag]) is not bool:
                raise ValueError('invalid server flag')
        if server['vpn'] == 'wireguard':
            key = server.get('wgpubkey')
            try:
                decoded = base64.b64decode(key, validate=True) if isinstance(key, str) else b''
                if len(decoded) != 32 or decoded == bytes(32) or base64.b64encode(decoded).decode() != key:
                    raise ValueError('invalid public key')
            except (ValueError, TypeError):
                raise ValueError('invalid public key') from None
    # Gluetun v3.41.3 flush.go sorts the server array when persisting it.
    # Bind every field and duplicate, but make identity independent of that
    # serialization order. Keep the snapshot's original candidate/IP ordering.
    canonical = dict(provider)
    canonical['servers'] = sorted(servers, key=lambda server: json.dumps(
        server, sort_keys=True, separators=(',', ':'), allow_nan=False))
    payload = json.dumps(canonical, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()
    if len(payload) > MAX_BYTES:
        raise ValueError('oversized catalog')
    catalog = Catalog(copy.deepcopy(provider), hashlib.sha256(payload).hexdigest(), timestamp)
    if not eligible_candidates(catalog, []):
        raise ValueError('no forwarding candidates')
    return catalog


def eligible_candidates(catalog, countries):
    """Empty countries means all eligible endpoints; explicit unmatched means none."""
    result, seen = [], set()
    for server in catalog.provider['servers']:
        if server['vpn'] != 'wireguard' or server.get('port_forward') is not True or server.get('secure_core') or server.get('free'):
            continue
        if countries and server['country'] not in countries:
            continue
        for ip in server['ips']:
            ip = str(ipaddress.ip_address(ip))
            if ip not in seen:
                result.append(Candidate(server['hostname'], server['country'], ip, server['wgpubkey']))
                seen.add(ip)
    return result


def _count(value):
    """A persisted repeat count; anything malformed reads as one failure."""
    if type(value) is not int or value < 1:
        return 1
    return min(value, REPEAT_COUNT_CAP)


def bounded_history(history, now):
    """Latest failure per endpoint, 24h retention, at most 256 entries; no mutation.

    An entry carries 'count' only when it is 2 or more. Duplicate records keep
    the highest count rather than adding up, so replaying or re-bounding a list
    never inflates it; only record_failure() increments.
    """
    if not isinstance(history, list) or not _finite(now):
        return []
    latest = {}
    # Bound CPU even for malformed persisted input. Runtime bounds JSON file size.
    for entry in history[-4096:]:
        if not isinstance(entry, dict):
            continue
        host, ip, failed = entry.get('hostname'), entry.get('endpoint_ip'), entry.get('failed_at')
        if not isinstance(host, str) or len(host) > 253 or not _finite(failed) or not 0 <= now-failed < 86400:
            continue
        try:
            ip = str(ipaddress.ip_address(ip))
        except (ValueError, TypeError):
            continue
        key = (host, ip)
        count = _count(entry.get('count', 1))
        if key in latest:
            count = max(count, latest[key].get('count', 1))
            failed = max(failed, latest[key]['failed_at'])
        latest[key] = {'hostname': host, 'endpoint_ip': ip, 'failed_at': failed}
        if count > 1:
            latest[key]['count'] = count
    return sorted(latest.values(), key=lambda e: (e['failed_at'], e['hostname'], e['endpoint_ip']))[-HISTORY_LIMIT:]


def record_failure(history, hostname, endpoint_ip, now):
    """History plus one failure of this endpoint; no mutation.

    The count grows while each failure lands within 24 h of the previous one
    (bounded_history's retention), so a quiet day starts it over. A record
    inside the previous one's exclusion window is the same incident (a failed
    rotation away and its escalation restart both record the server): it
    moves failed_at, extending the exclusion, but does not add to the count.
    """
    current = bounded_history(history, now)
    try:
        ip = str(ipaddress.ip_address(endpoint_ip))
    except (ValueError, TypeError):
        return current
    prior = next((e for e in current if e['hostname'] == hostname and e['endpoint_ip'] == ip), None)
    entry = {'hostname': hostname, 'endpoint_ip': ip, 'failed_at': now}
    count = 1
    if prior:
        count = prior.get('count', 1)
        if now-prior['failed_at'] >= EXCLUSION_S:
            count = min(REPEAT_COUNT_CAP, count+1)
    if count > 1:
        entry['count'] = count
    return bounded_history([e for e in current if e is not prior]+[entry], now)


def _network(ip):
    """The block a failure is likely to share: IPv4 /24, IPv6 /48."""
    try:
        address = ipaddress.ip_address(ip)
    except (ValueError, TypeError):
        return None
    return ipaddress.ip_network(f'{address}/{24 if address.version == 4 else 48}', strict=False)


def select_candidate(candidates, history, preferred_countries, fallback_countries, now):
    """Pick without mutation. All-excluded returns oldest; caller MUST back off.

    history is {failures: [{hostname, endpoint_ip, failed_at}],
    round_attempts: [hostname, ...]}. A legacy list means failures with an empty
    round. Runtime resets round_attempts after the failed-round backoff, or once
    a recovered selection has been continuously Ready for 30 minutes.
    """
    failures = bounded_history(history.get('failures', []) if isinstance(history, dict) else history, now)
    attempts = history.get('round_attempts', []) if isinstance(history, dict) else []
    attempts = set(x for x in attempts[:256] if isinstance(x, str)) if isinstance(attempts, list) else set()
    preferred = [c for c in candidates if c.country in preferred_countries]
    fallback = [c for c in candidates if c.country in fallback_countries and c not in preferred]
    tried = {c.hostname for c in preferred if c.hostname in attempts}
    recent = {e['endpoint_ip']: e['failed_at'] for e in failures if now-e['failed_at'] < EXCLUSION_S}
    # Proton lists same-block neighbours together (node-ca-59/60 are both
    # 84.20.16.x), and a site-wide fault takes the whole block. Within a pool,
    # prefer a server outside every block that just failed or was tried this
    # round; never leave the pool (country) just to find one.
    avoid = {_network(ip) for ip in recent}
    avoid |= {_network(c.endpoint_ip) for c in candidates if c.hostname in attempts}
    avoid.discard(None)
    # A server that keeps failing goes after every other eligible one: once
    # its 30-minute exclusion lapsed, node-us-119 (first in catalog order,
    # never leased a port) was picked again and again, and leaving it hung
    # Gluetun's settings request for two minutes each time (2026-11-15).
    repeat = {e['endpoint_ip'] for e in failures if e.get('count', 1) >= REPEAT_FAILURES}
    pools = [preferred, fallback] if len(tried) < 2 else [fallback]
    for pool in pools:
        eligible = [c for c in pool if c.hostname not in attempts and c.endpoint_ip not in recent]
        if eligible:
            # min() keeps catalog order among equals.
            return min(eligible, key=lambda c: (c.endpoint_ip in repeat, _network(c.endpoint_ip) in avoid))
    all_allowed = preferred+fallback
    # Already attempted servers require a new round even if a failure is absent.
    if all_allowed and all(c.endpoint_ip in recent for c in all_allowed):
        return min(all_allowed, key=lambda c: (recent[c.endpoint_ip], c.hostname, c.endpoint_ip))
    return None


def _fetch_https(url, headers, timeout_s, max_bytes):
    """Static HTTPS only, no redirects/account API; deadline covers all reads."""
    parsed = urlsplit(url)
    if url != CATALOG_URL or parsed.scheme != 'https':
        raise ValueError('unapproved catalog source')
    deadline = time.monotonic()+timeout_s
    connection = http.client.HTTPSConnection(parsed.hostname, timeout=timeout_s)
    try:
        connection.request('GET', parsed.path, headers=headers)
        sock = connection.sock
        sock.settimeout(max(.001, deadline-time.monotonic()))
        response = connection.getresponse()
        chunks, size = [], 0
        while response.status == 200:
            remaining = deadline-time.monotonic()
            if remaining <= 0:
                raise TimeoutError('catalog fetch timeout')
            sock.settimeout(remaining)
            chunk = response.read1(min(65536, max_bytes+1-size))
            if not chunk:
                break
            chunks.append(chunk)
            size += len(chunk)
            if size > max_bytes:
                raise ValueError('oversized catalog')
        return FetchResponse(response.status, b''.join(chunks), dict(response.getheaders()))
    finally:
        connection.close()


_FETCH_WORKER = '''
import json, sys
sys.path.insert(0, sys.argv[1])
from vpn_recovery.catalog import _fetch_https
url, headers, timeout, limit = json.load(sys.stdin)
response = _fetch_https(url, headers, timeout, limit)
headers = {k: v for k, v in response.headers.items()
           if k.lower() in ('etag', 'last-modified') and len(v) <= 1024}
sys.stdout.buffer.write(json.dumps([response.status, headers]).encode()+b'\\n'+response.body)
'''


def fetch_public_catalog(url, headers, timeout_s, max_bytes):
    """Bound even libc DNS in an isolated worker: 15s + at most 1s kill/reap.

    The child creates no descendants; it emits only bounded public metadata.
    -I excludes user/site PYTHONPATH customizations from the short-lived worker.
    """
    if url != CATALOG_URL or not _finite(timeout_s) or timeout_s <= 0 or not 0 < max_bytes <= MAX_BYTES:
        raise ValueError('invalid catalog request')
    start = time.monotonic()
    process = subprocess.Popen([sys.executable, '-I', '-c', _FETCH_WORKER, str(Path(__file__).resolve().parents[1])],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        try:
            output, _ = process.communicate(json.dumps([url, headers, timeout_s, max_bytes]).encode(),
                                            timeout=max(.001, timeout_s-(time.monotonic()-start)))
        except subprocess.TimeoutExpired:
            process.kill()
            try:
                process.communicate(timeout=1)
            except subprocess.TimeoutExpired:
                pass
            raise TimeoutError('catalog fetch timeout') from None
        if process.returncode != 0 or len(output) > max_bytes+4096:
            raise ValueError('catalog fetch failed')
        metadata, separator, body = output.partition(b'\n')
        if not separator or len(metadata) > 4096 or len(body) > max_bytes:
            raise ValueError('invalid catalog response')
        status, response_headers = _json(metadata)
        if type(status) is not int or not isinstance(response_headers, dict):
            raise ValueError('invalid catalog response')
        return FetchResponse(status, body, response_headers)
    finally:
        for stream in (process.stdin, process.stdout):
            if stream is not None:
                stream.close()


def _read(path, limit):
    with Path(path).open('rb') as stream:
        raw = stream.read(limit+1)
    if len(raw) > limit:
        raise ValueError('oversized catalog storage')
    return raw


def _preserving_write(path, raw, metadata):
    """Stage ownership/mode before publication, then fsync file and directory."""
    fd, temporary = tempfile.mkstemp(prefix='.'+path.name+'.', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            current = os.fstat(stream.fileno())
            if (current.st_uid, current.st_gid) != (metadata.st_uid, metadata.st_gid):
                os.fchown(stream.fileno(), metadata.st_uid, metadata.st_gid)
            os.fchmod(stream.fileno(), stat.S_IMODE(metadata.st_mode))
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


class CatalogStore:
    def __init__(self, paths, fetcher=fetch_public_catalog, *, jitter=None, clock=time.time):
        self.paths, self.fetcher, self.clock = paths, fetcher, clock
        self.jitter = jitter if jitter is not None else lambda: random.SystemRandom().randint(-1800, 1800)
        self.path = paths.services/'config/gluetun/servers.json'
        self.state_path = paths.state/'catalog.json'

    def _load(self, now):
        raw = _read(self.path, 32*1024*1024)
        document = _json(raw)
        if not isinstance(document, dict):
            raise ValueError('invalid catalog document')
        catalog = validate_catalog(document.get('protonvpn'), now)
        return raw, document, catalog

    def load(self):
        """Downloaded snapshot only, NOT the loaded catalog of any container."""
        return self._load(self.clock())[2]

    def _state(self, now):
        try:
            state = _json(_read(self.state_path, 16384))
            if (not isinstance(state, dict) or state.get('version') != 1
                    or not _finite(state.get('checked_at')) or not _finite(state.get('next_due'))
                    or not 0 <= now-state['checked_at'] < 30*86400
                    or not 0 < state['next_due']-state['checked_at'] <= 88200
                    or not re.fullmatch('[a-f0-9]{64}', state.get('downloaded_identity', ''))):
                return {}
            for key in ('etag', 'last_modified'):
                value = state.get(key, '')
                if not isinstance(value, str) or len(value) > 1024 or any(ord(c) < 32 or ord(c) > 126 for c in value):
                    return {}
            return state
        except (OSError, ValueError, TypeError, RecursionError):
            return {}

    def refresh(self, now, force=False):
        """Fetch outside compose lock; validate/merge again under it. Never restart."""
        if not _finite(now):
            return {'status': 'failed', 'reason': 'invalid_clock'}
        state = self._state(now)
        result = {'status': 'failed', 'reason': 'catalog_unavailable'}
        try:
            before = self._load(now)[2]
            if not force and state.get('next_due', 0) > now and state.get('downloaded_identity') == before.identity:
                return {'status': 'not_due', **{k: state[k] for k in ('downloaded_identity', 'next_due')}}
            headers = {}
            if state.get('downloaded_identity') == before.identity:
                for source, target in [('etag', 'If-None-Match'), ('last_modified', 'If-Modified-Since')]:
                    if state.get(source):
                        headers[target] = state[source]
            response = self.fetcher(CATALOG_URL, headers, 15, MAX_BYTES)
            if response.status == 200:
                if not isinstance(response.body, bytes) or len(response.body) > MAX_BYTES:
                    raise ValueError('oversized response')
                downloaded = validate_catalog(_json(response.body), now)
            elif response.status == 304 and headers:
                downloaded = before
            else:
                raise ValueError('catalog fetch refused')
            try:
                with bounded_lock(self.paths.compose_lock, 1):
                    raw, document, current = self._load(now)
                    downloaded = validate_catalog(downloaded.provider, self.clock())
                    policy = load_config(self.paths)
                    if not eligible_candidates(downloaded, policy['preferred_countries']):
                        raise ValueError('empty preferred pool')
                    if response.status == 304 and current.identity != before.identity:
                        raise ValueError('stale conditional response')
                    if downloaded.timestamp < current.timestamp or (downloaded.timestamp == current.timestamp and downloaded.identity != current.identity):
                        raise ValueError('stale catalog writer')
                    changed = current.identity != downloaded.identity
                    if changed:
                        metadata = self.path.stat()
                        _preserving_write(self.path.with_name('servers.json.known-good'), raw, metadata)
                        document['protonvpn'] = downloaded.provider
                        _preserving_write(self.path, (json.dumps(document, sort_keys=True, allow_nan=False)+'\n').encode(), metadata)
                    jitter = self.jitter()
                    jitter = max(-1800, min(1800, jitter)) if type(jitter) is int else 0
                    new_state = {'version': 1, 'checked_at': now, 'next_due': now+86400+jitter,
                                 'downloaded_identity': downloaded.identity}
                    response_headers = {k.lower(): v for k, v in response.headers.items()}
                    for key, source in [('etag', 'etag'), ('last_modified', 'last-modified')]:
                        value = response_headers.get(source, state.get(key, '') if response.status == 304 else '')
                        if isinstance(value, str) and len(value) <= 1024 and all(32 <= ord(c) <= 126 for c in value):
                            new_state[key] = value
                    atomic_json(self.state_path, new_state)
                    return {'status': 'updated' if changed else 'not_modified',
                            'downloaded_identity': downloaded.identity, 'timestamp': downloaded.timestamp,
                            'next_due': new_state['next_due'], 'requires_load': changed}
            except (TimeoutError, FileNotFoundError, PermissionError):
                return {'status': 'deferred', 'reason': 'lock_or_storage_unavailable'}
        except (OSError, ValueError, TypeError, KeyError, RecursionError, http.client.HTTPException):
            return result
