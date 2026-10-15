"""manager.js escapeJs() must be safe inside onclick="fn('${escapeJs(x)}')".

Every caller embeds the value in a single-quoted JS string INSIDE a
double-quoted HTML attribute. The HTML parser runs first: a raw `"` ends
the attribute early, and `&#39;` decodes back to `'` before JS ever sees
it. A ROM or playlist filename from a shared package can carry either, so
the escaper must emit neither — only JS escapes that are inert as HTML.

Runs the real function from manager.js under node; skipped where node is
absent.
"""

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

MANAGER_JS = Path(__file__).resolve().parents[1] / "static" / "manager.js"

HOSTILE = [
    'x" onmouseover="alert(1)" y=".bin',
    "x&#39;);alert(1);//.bin",
    "x');alert(1);//.bin",
    "x\\');alert(1);//",
    "a<b>c</script>.bin",
    "line\nbreak\r  .bin",
    "plain name (USA).zip",
]


def _escape_js_source() -> str:
    src = MANAGER_JS.read_text(encoding="utf-8")
    m = re.search(r"^function escapeJs\(str\) \{.*?^\}", src, re.S | re.M)
    assert m, "escapeJs not found in manager.js"
    return m.group(0)


@pytest.fixture(scope="module")
def escaped():
    node = shutil.which("node")
    if node is None:
        pytest.skip("node not installed")
    script = (
        _escape_js_source()
        + "\nconst inputs = JSON.parse(require('fs').readFileSync(0, 'utf8'));"
        + "\nprocess.stdout.write(JSON.stringify(inputs.map(escapeJs)));"
    )
    out = subprocess.run(
        [node, "-e", script], input=json.dumps(HOSTILE),
        capture_output=True, text=True, check=True,
    )
    return dict(zip(HOSTILE, json.loads(out.stdout)))


@pytest.mark.parametrize("raw", HOSTILE)
def test_output_has_no_html_significant_characters(escaped, raw):
    # Nothing the HTML attribute parser acts on may survive: no quote to
    # end the attribute, no ampersand to start an entity, no angle bracket.
    assert not set('"&<>') & set(escaped[raw])


@pytest.mark.parametrize("raw", HOSTILE)
def test_round_trips_through_a_js_string_literal(escaped, raw):
    # The JS engine must decode the escaped text back to the exact original
    # (deleteROM etc. send it to the server as the path to act on).
    node = shutil.which("node")
    literal = "'" + escaped[raw] + "'"
    out = subprocess.run(
        [node, "-e", f"process.stdout.write(JSON.stringify({literal}))"],
        capture_output=True, text=True, check=True,
    )
    assert json.loads(out.stdout) == raw
