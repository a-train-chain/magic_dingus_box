import os
from pathlib import Path
from magic_dingus_box.web.admin import create_app

# Default data directory
DATA_DIR = Path(os.getenv("MAGIC_DATA_DIR", "/opt/magic_dingus_box/magic_dingus_box_cpp/data"))

app = create_app(DATA_DIR)

if __name__ == "__main__":
    # threaded=True is REQUIRED, not an optimization: the Phone Remote's
    # WebSocket route (flask-sock) runs a while-True receive loop for the
    # whole life of the connection. Werkzeug's default is threaded=False —
    # ONE worker — so a single connected phone remote used to occupy the
    # only worker and every other request (playlist edits, uploads,
    # pairing a second phone, the whole Content Manager) hung until that
    # phone disconnected. With no WS heartbeat a phone that dropped off
    # WiFi uncleanly could hold the worker for hours (kernel TCP-keepalive
    # timescale). flask-sock's own docs call out that threading must be
    # enabled for exactly this reason.
    app.run(host="0.0.0.0", port=5000, threaded=True)
