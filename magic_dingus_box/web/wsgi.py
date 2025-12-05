import os
from pathlib import Path
from magic_dingus_box.web.admin import create_app

# Default data directory
DATA_DIR = Path(os.getenv("MAGIC_DATA_DIR", "/opt/magic_dingus_box/magic_dingus_box_cpp/data"))

app = create_app(DATA_DIR)

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
