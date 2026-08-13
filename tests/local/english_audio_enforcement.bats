#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# English-audio enforcement (owner decision 2026-08-13): "no foreign audio
# on English content" — a release carrying foreign-dub / multi-audio markers,
# or whose language is not the content's ORIGINAL language, must be rejected;
# a genuinely foreign film must STILL be downloadable in its own language.
#
# Two Custom Formats implement it, and BOTH are load-bearing:
#
#   Layer 1  "Release language is not the original" — LanguageSpecification
#            value=-2 (Original) + exceptLanguage=true. Precise, but it
#            trusts the release parser.
#   Layer 2  "Non-English title signals" — title regex. Required because the
#            parser is exactly what failed: it tagged
#            "Game Of Thrones S03 MULTi (1080p) BluRay x264 PopHD" (French
#            audio) as English — i.e. as the Original — so layer 1 alone
#            would have passed it, and it was grabbed.
#
# These tests lock the shipped fixtures against the two ways this can rot:
# the regex losing the marker that caused the incident, and the score map
# losing an entry (the reconciler only rescores formats it names, so a
# missing SCORE_MAP line silently leaves boxes unprotected).

RADARR_CF="$CPP_DIR/scripts/data/radarr_custom_formats.json"
SONARR_CF="$CPP_DIR/scripts/data/sonarr_custom_formats.json"
SETUP_SH="$CPP_DIR/scripts/setup_services.sh"

@test "layer 2 regex: rejects the GoT MULTi release, keeps clean English + world cinema" {
    run python3 - "$RADARR_CF" <<'PY'
import json, re, sys

cfs = json.load(open(sys.argv[1]))
cf = next(c for c in cfs if c["name"] == "Non-English title signals")
spec = cf["specifications"][0]
pattern = next(f["value"] for f in spec["fields"] if f["name"] == "value")
# Radarr/Sonarr compile these case-insensitively.
rx = re.compile(pattern, re.IGNORECASE)

MUST_MATCH = [
    # The incident release. Bare MULTi, no dub keyword, Latin script only.
    "Game Of Thrones S03 MULTi (1080p) BluRay x264 PopHD",
    "Game.of.Thrones.S03.MULTI.1080p.BluRay.x264-GROUP",
    "Some.Movie.2019.MULTI-VF.1080p.WEB-DL.x264",
    "Some.Movie.2019.TRUEFRENCH.1080p.BluRay.x264",
    "Some.Show.S01.VOSTFR.1080p.WEB-DL.x264",
    "Some.Movie.2019.SUBFRENCH.1080p.BluRay.x264",
    "Some.Movie.2019.VFF.1080p.BluRay.x264",
    "Some.Movie.2019.VFQ.1080p.WEB-DL.x264",
    "Some.Movie.2019.Dual.Audio.1080p.BluRay.x264",
    "Some.Movie.2019.DUAL-AUDIO.1080p.x264",
    "Some.Movie.2019.MULTI.AUDIO.1080p.x264",
    "Some.Movie.2019.GERMAN.DL.1080p.BluRay.x264",
    "Some.Movie.2019.iTA.ENG.1080p.BluRay.x264",
    "Some.Movie.2019.1080p.HIN-TAM-TEL.WEB-DL.x264",
    "Some.Movie.2019.1080p.BluRay.x264.DUBBED",
    "Some.Movie.2019.1080p.BluRay.x264.Italian.Dub",
    # Non-Latin script (the original signal, must survive the broadening).
    "Дьявол носит Prada 2 / The Devil Wears Prada 2",
]

# World cinema must stay obtainable in its ORIGINAL language: a bare
# single-language token marks the original, so layer 2 must not fire on it.
# (Layer 1 is what rejects those same tokens on English-original content.)
MUST_NOT_MATCH = [
    "Parasite 2019 1080p BluRay x264",
    "Parasite.2019.KOREAN.1080p.BluRay.x264-GROUP",
    "Amelie.2001.FRENCH.1080p.BluRay.x264-GROUP",
    "Spirited.Away.2001.JAPANESE.1080p.BluRay.x264",
    "Cinema.Paradiso.1988.ITALIAN.1080p.BluRay.x264",
    "Das.Boot.1981.GERMAN.1080p.BluRay.x264",
    # Ordinary English releases, including traps for the token classes.
    "The.Matrix.1999.1080p.BluRay.x264-AMIABLE",
    "Whiplash.2014.1080p.BluRay.x264-SPARKS",          # "SPA" inside SPARKS
    "Multiplicity.1996.1080p.WEB-DL.x264",             # "multi" inside a word
    "Deep.Water.2022.MULTI-SUBS.1080p.WEB-DL.x264",    # subtitles, not audio
    "Game.of.Thrones.S03.1080p.BluRay.x264-ROVERS",
    "Dublin.Murders.S01.1080p.WEB-DL.x264",            # "dub" inside Dublin
]

bad = []
for t in MUST_MATCH:
    if not rx.search(t):
        bad.append("SHOULD MATCH but did not: " + t)
for t in MUST_NOT_MATCH:
    m = rx.search(t)
    if m:
        bad.append("SHOULD NOT MATCH but did (%r): %s" % (m.group(0), t))
if bad:
    print("\n".join(bad))
    sys.exit(1)
print("OK: %d match / %d no-match titles behave" % (len(MUST_MATCH), len(MUST_NOT_MATCH)))
PY
    [ "$status" -eq 0 ] || { echo "$output"; false; }
}

@test "layer 1 spec: Original + exceptLanguage, and never both negate and exceptLanguage" {
    run python3 - "$RADARR_CF" "$SONARR_CF" <<'PY'
import json, sys

problems = []
for path in sys.argv[1:]:
    cfs = json.load(open(path))
    cf = next((c for c in cfs if c["name"] == "Release language is not the original"), None)
    if cf is None:
        problems.append("%s: format missing" % path)
        continue
    spec = cf["specifications"][0]
    fields = {f["name"]: f["value"] for f in spec["fields"]}
    if spec["implementation"] != "LanguageSpecification":
        problems.append("%s: implementation=%s" % (path, spec["implementation"]))
    if fields.get("value") != -2:
        problems.append("%s: value=%r (want -2 = Original)" % (path, fields.get("value")))
    if fields.get("exceptLanguage") is not True:
        problems.append("%s: exceptLanguage=%r (want true)" % (path, fields.get("exceptLanguage")))
    # exceptLanguage IS the inversion ("matches if any language other than
    # the selected one is present"). negate on top double-negates it into
    # "reject everything that is purely the original language" — which
    # rejects every good release instead of every bad one.
    if spec.get("negate"):
        problems.append("%s: negate AND exceptLanguage both set (double negation)" % path)
    if not spec.get("required"):
        problems.append("%s: required=false" % path)

if problems:
    print("\n".join(problems))
    sys.exit(1)
print("OK: layer-1 spec correct in both fixtures")
PY
    [ "$status" -eq 0 ] || { echo "$output"; false; }
}

@test "both fixtures define identical specifications for every format" {
    # The parity test next door compares NAMES. That would not have caught a
    # Radarr-only regex retune, which is precisely how the two services drift
    # into scoring against different rules.
    run python3 - "$RADARR_CF" "$SONARR_CF" <<'PY'
import json, sys

def by_name(path):
    return {c["name"]: c["specifications"] for c in json.load(open(path))}

r, s = by_name(sys.argv[1]), by_name(sys.argv[2])
problems = []
for name in sorted(set(r) & set(s)):
    if r[name] != s[name]:
        problems.append("specifications differ between fixtures: " + name)
if problems:
    print("\n".join(problems))
    sys.exit(1)
print("OK: %d formats have identical specs in both fixtures" % len(set(r) & set(s)))
PY
    [ "$status" -eq 0 ] || { echo "$output"; false; }
}

@test "every fixture format is scored in BOTH setup_services.sh SCORE_MAPs" {
    # The profile reconciler only rescores formats NAMED in its SCORE_MAP.
    # A format that ships in the fixture but is missing from a score map is
    # created on the box at score 0 — present, visible in the UI, and doing
    # nothing. That is the silent-unprotected-box failure mode.
    run python3 - "$SETUP_SH" "$RADARR_CF" "$SONARR_CF" <<'PY'
import json, re, sys

setup = open(sys.argv[1]).read()
blocks = re.findall(r"^SCORE_MAP = \{(.*?)^\}", setup, re.S | re.M)
if len(blocks) != 2:
    print("expected exactly 2 SCORE_MAP blocks in setup_services.sh, found %d" % len(blocks))
    sys.exit(1)

def names_in(block):
    return set(re.findall(r'^\s*"([^"]+)":', block, re.M))

radarr_map, sonarr_map = names_in(blocks[0]), names_in(blocks[1])
fixture_names = {c["name"] for c in json.load(open(sys.argv[2]))}
fixture_names |= {c["name"] for c in json.load(open(sys.argv[3]))}

problems = []
for label, mapping in (("Radarr", radarr_map), ("Sonarr", sonarr_map)):
    missing = fixture_names - mapping
    if missing:
        problems.append("%s SCORE_MAP missing: %s" % (label, ", ".join(sorted(missing))))

# The two rejection layers must both sit below the -200 floor on their own.
for label, block in (("Radarr", blocks[0]), ("Sonarr", blocks[1])):
    for fmt in ("Non-English title signals", "Release language is not the original"):
        m = re.search(r'^\s*"%s":\s*(-?\d+),' % re.escape(fmt), block, re.M)
        if not m:
            problems.append("%s SCORE_MAP has no score for %r" % (label, fmt))
        elif int(m.group(1)) > -200:
            problems.append("%s %r scored %s (must be <= -200 to reject alone)"
                            % (label, fmt, m.group(1)))

if problems:
    print("\n".join(problems))
    sys.exit(1)
print("OK: %d fixture formats scored in both maps" % len(fixture_names))
PY
    [ "$status" -eq 0 ] || { echo "$output"; false; }
}
