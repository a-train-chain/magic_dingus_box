#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

@test "forbidden video formats remain rejected with every positive preference" {
    run python3 - "$CPP_DIR/scripts/converge_custom_formats.sh" <<'PY'
import ast, itertools, pathlib, re, sys
source = pathlib.Path(sys.argv[1]).read_text()
maps = re.findall(r'^SCORE_MAP = (\{.*?^\})', source, re.M | re.S)
assert len(maps) == 2, 'exercise both Radarr and Sonarr policies'
problems = []
for service, literal in zip(('Radarr', 'Sonarr'), maps):
    scores = ast.literal_eval(literal)
    positives = [s for s in scores.values() if s > 0]
    for name in ('AV1 codec (UNWATCHABLE on Pi 4)', 'x265/HEVC 1080p+',
                 'HDR / Dolby Vision', 'Remux / Raw-HD'):
        for bits in itertools.product((0, 1), repeat=len(positives)):
            total = scores[name] + sum(s for s, enabled in zip(positives, bits) if enabled)
            if total >= -200:
                problems.append(f'{service}: {name} qualifies at {total}')
    assert scores['x264 codec (BONUS)'] >= -200
    assert scores['x264 codec (BONUS)'] + scores['Low-bitrate size-optimized groups'] >= -200
assert not problems, '; '.join(problems)
PY
    [ "$status" -eq 0 ] || { echo "$output"; false; }
}

@test "live profile verification rejects unsafe codec penalties and accepts safe scores" {
    run python3 - "$CPP_DIR/scripts/verify_services.sh" <<'PY'
import json, pathlib, re, subprocess, sys
source = pathlib.Path(sys.argv[1]).read_text()
blocks = [s for s in re.findall(r"<<'PYEOF'\n(.*?)\nPYEOF", source, re.S)
          if 'minFormatScore' in s and 'expected_allowed' in s]
assert len(blocks) == 2
allowed = ['HDTV-720p', 'WEBDL-720p', 'WEBRip-720p', 'Bluray-720p',
           'HDTV-1080p', 'WEBDL-1080p', 'WEBRip-1080p', 'Bluray-1080p']
base = {'name': 'Any', 'minFormatScore': -200, 'cutoff': 4,
        'items': [{'allowed': True, 'quality': {'id': i + 1, 'name': n}}
                  for i, n in enumerate(allowed)]}
for block in blocks:
    for penalty, extra_bonus, missing, want in (
            (-200, 0, False, 'FAIL:'), (-250, 0, False, 'FAIL:'),
            (-500, 0, False, 'OK:'), (-500, 400, False, 'FAIL:'),
            (-500, 0, True, 'FAIL:')):
        scores = {'AV1 codec (UNWATCHABLE on Pi 4)': -1000,
                  'x265/HEVC 1080p+': penalty, 'HDR / Dolby Vision': penalty,
                  'Remux / Raw-HD': -500, 'x264 codec (BONUS)': 50,
                  'Quality release groups': 30}
        if extra_bonus:
            scores['Operator preference'] = extra_bonus
        if missing:
            del scores['HDR / Dolby Vision']
        profile = dict(base, formatItems=[{'name': n, 'score': v} for n, v in scores.items()])
        result = subprocess.run([sys.executable, '-c', block, json.dumps([profile]),
                                 'Any', '-200', 'Bluray-720p', *allowed],
                                capture_output=True, text=True, check=True)
        assert result.stdout.startswith(want), (penalty, want, result.stdout)
PY
    [ "$status" -eq 0 ] || { echo "$output"; false; }
}
