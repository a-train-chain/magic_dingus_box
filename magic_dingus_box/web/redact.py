"""Secret scrubbing for anything the Content Manager hands out for support.

Used by the diagnostics bundle (journal lines, unit status, launcher logs)
and, defensively, by the Box health card (verify_box.sh output is designed
to be secret-free, but its --with-services tail quotes verify_services.sh,
which talks to qBittorrent with the real password).

Three layers, applied per line:

1. KNOWN values — the caller passes the box's actual secrets (every value
   in services/.env, the Flask HMAC key, the TMDB key) and each exact
   occurrence is replaced. This is the only layer that can catch a secret
   with no recognisable shape, e.g. qBittorrent's 24-char random password
   floating in a log line with no "password=" in front of it.
2. SHAPES — KEY=value env assignments, "key": "value" JSON, `X-Api-Key:` /
   `Authorization:` / `Cookie:` headers, `?apikey=` URL parameters, bearer
   tokens, JWTs (TMDB v4 tokens), WireGuard base64 keys and long hex
   strings (*arr / TMDB v3 API keys, the Flask secret, the phone-remote
   cookie signature). Replaced in place; the rest of the line is kept,
   because the rest is usually the diagnosis.
3. WHOLE LINE (``strict=True``) — a line that still mentions a password /
   psk / secret / token / private key with nothing redacted right after
   the word is dropped, e.g. `wifi connect Home password hunter2`.
   Over-redaction is the accepted failure mode: a support bundle missing a
   line is an inconvenience, one carrying a household's Wi-Fi password is
   an incident.
"""
from __future__ import annotations

import re
from typing import Iterable, Optional

REDACTED = "[REDACTED]"
LINE_REDACTED = "[line redacted: it mentioned a {what}]"

# Words that make the VALUE of an assignment (`NAME=value`) secret. Broad on
# purpose: MDB_QBIT_PASS, SONARR_API_KEY, WIREGUARD_PRIVATE_KEY, psk=,
# auth_token=, ... An assignment is cheap to over-redact.
_ASSIGN_WORDS = (r"pass|pwd|psk|secret|token|key|auth|cookie|credential|"
                 r"private|session|sig")
# Words that make the value after a `word: value` colon secret. Narrower —
# prose with colons ("[PASS] board: Raspberry Pi 5") must survive.
_COLON_WORDS = (r"password|passwd|passphrase|psk|secret|token|api[ _-]?key|"
                r"apikey|private[ _-]?key|privatekey|preshared[ _-]?key")

_VALUE_RULES = [
    # URL query parameters: ?apikey=..., &token=..., &password=...
    (re.compile(r"(?i)([?&;](?:api[_-]?key|apikey|token|access_token|"
                r"password|passwd|pass|key|secret|sig|signature|code|pair)=)"
                r"[^&\s\"'<>]+"),
     r"\1" + REDACTED),
    # HTTP-ish headers. Everything after the separator is the credential.
    (re.compile(r"(?i)\b(x-api-key|x-csrf-token|x-magic-token|authorization|"
                r"proxy-authorization|set-cookie|cookie)(\s*[:=]\s*)\S.*"),
     r"\1\2" + REDACTED),
    # Bearer / Basic tokens anywhere in a line.
    (re.compile(r"(?i)\b(bearer|basic)\s+[A-Za-z0-9._~+/=-]{8,}"),
     r"\1 " + REDACTED),
    # JSON "name": "value" where the NAME looks secret.
    (re.compile(r"(?i)(\"[^\"\n]*(?:" + _ASSIGN_WORDS + r")[^\"\n]*\"\s*:\s*)"
                r"(\"(?:[^\"\\\n]|\\.)*\"|[^\s,}\]]+)"),
     r'\1"' + REDACTED + '"'),
    # ENV / ini assignments: WIREGUARD_PRIVATE_KEY=..., api_key = ..., psk=...
    (re.compile(r"(?i)\b([A-Za-z0-9_.-]*(?:" + _ASSIGN_WORDS +
                r")[A-Za-z0-9_.-]*)(\s*=\s*)(\"[^\"]*\"|'[^']*'|[^\s,;&]+)"),
     r"\1\2" + REDACTED),
    # "password: value", "psk: value", "api key: value".
    (re.compile(r"(?i)\b((?:" + _COLON_WORDS + r"))(\s*:\s*)(\"[^\"]*\"|'[^']*'|\S+)"),
     r"\1\2" + REDACTED),
    # JWT (TMDB v4 read-access tokens are JWTs).
    (re.compile(r"\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}"),
     REDACTED),
    # WireGuard keys: 32 bytes base64 = 43 chars + "=".
    (re.compile(r"(?<![A-Za-z0-9+/])[A-Za-z0-9+/]{42,43}=(?![A-Za-z0-9+/=])"),
     REDACTED),
    # Long hex: *arr API keys (32), TMDB v3 key (32), Flask secret (64),
    # HMAC signatures (64). Also catches some harmless ids — accepted.
    (re.compile(r"(?i)(?<![0-9a-z])[0-9a-f]{32,}(?![0-9a-z])"),
     REDACTED),
    # token_urlsafe()-style blobs: 32+ url-safe chars mixing letters and
    # digits. No "/" or "." in the class, so file paths survive.
    (re.compile(r"(?<![A-Za-z0-9_-])(?=[A-Za-z0-9_-]*[0-9])(?=[A-Za-z0-9_-]*[A-Za-z])"
                r"[A-Za-z0-9_-]{32,}(?![A-Za-z0-9_-])"),
     REDACTED),
]

_LINE_WORDS = re.compile(
    r"(?i)(pass(?:word|wd|phrase)|\bpsk\b|wpa-?psk|\bsecret|\btoken\b|"
    r"private[ _-]?key|privatekey|api[ _-]?key|apikey)")


_NAMES = re.compile(
    r"[\w@.-]+\.(?:service|timer|socket|mount|target|sh|py|log|json|conf|"
    r"key|env|yml|yaml)\b")


class Redactor:
    """`known` = exact secret values to scrub wherever they appear."""

    MIN_KNOWN_LEN = 6

    def __init__(self, known: Optional[Iterable[str]] = None):
        def usable(v):
            # A short plain word or number ("enabled", "1000") under a
            # secret-looking name is a setting, not a credential, and
            # scrubbing it everywhere would shred ordinary log text.
            return (isinstance(v, str) and len(v) >= self.MIN_KNOWN_LEN
                    and not v.isdigit() and not (v.isalpha() and len(v) <= 12))
        vals = sorted({v.strip() for v in (known or []) if usable((v or "").strip())},
                      key=len, reverse=True)
        self._known = re.compile("|".join(map(re.escape, vals))) if vals else None

    def line(self, line: str, strict: bool = True) -> str:
        out = self._known.sub(REDACTED, line) if self._known else line
        for rx, repl in _VALUE_RULES:
            out = rx.sub(repl, out)
        if strict:
            # File and unit NAMES are not secrets: without this, every
            # journal/systemctl line about magic-dingus-sync-qbit-password
            # .service or flask_secret.key would be dropped.
            probe = _NAMES.sub(" ", out)
            for m in _LINE_WORDS.finditer(probe):
                # Handled if a redaction marker follows the word closely
                # ("password=[REDACTED]", "X-Api-Key: [REDACTED]").
                if REDACTED not in probe[m.end():m.end() + 24]:
                    return LINE_REDACTED.format(what=m.group(1).lower())
        return out

    def text(self, text: str, strict: bool = True) -> str:
        """Redact every line of `text`, preserving line endings."""
        if not text:
            return text
        parts = []
        for chunk in text.splitlines(keepends=True):
            body = chunk.rstrip("\r\n")
            parts.append(self.line(body, strict) + chunk[len(body):])
        return "".join(parts)


_DEFAULT = Redactor()


def redact_line(line: str, strict: bool = True) -> str:
    return _DEFAULT.line(line, strict)


def redact_text(text: str, strict: bool = True) -> str:
    return _DEFAULT.text(text, strict)


_SECRET_NAME = re.compile(r"(?i)" + _ASSIGN_WORDS)


def env_secret_values(env_text: str) -> list:
    """Secret values from a services/.env-style file: every value whose NAME
    looks secret, plus any long opaque value (16+ chars, no path separator
    or space) whatever its name — a credential under an unexpected name.
    Plain settings (TZ=America/New_York, STORAGE_ROOT=/mnt/ssd) are NOT
    returned: scrubbing "/mnt/ssd" from every log line would erase exactly
    what a storage problem needs to show."""
    out = []
    for raw in (env_text or "").splitlines():
        line = raw.strip()
        if line.startswith("export "):
            line = line[len("export "):].strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        name, value = (part.strip() for part in line.split("=", 1))
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        if not value:
            continue
        opaque = (len(value) >= 16 and "/" not in value and " " not in value
                  and not value.isdigit())
        if _SECRET_NAME.search(name) or opaque:
            out.append(value)
    return out
