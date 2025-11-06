from __future__ import annotations

import logging
from logging.handlers import RotatingFileHandler
from pathlib import Path

from .config import AppConfig


def setup_logging(config: AppConfig, verbose: bool = False) -> None:
    """Configure rotating file logging with console fallback.

    Attempts to write to `/data/logs/magic-ui.log` (or dev_data on macOS). If that
    fails, falls back to stderr-only logging to ensure visibility.
    """

    root_logger = logging.getLogger()
    root_logger.setLevel(logging.DEBUG if verbose else logging.INFO)

    formatter = logging.Formatter(
        fmt="%(asctime)s %(levelname)s [%(name)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    # Always attach a console handler for early visibility
    stream_handler = logging.StreamHandler()
    stream_handler.setLevel(logging.DEBUG if verbose else logging.INFO)
    stream_handler.setFormatter(formatter)
    root_logger.addHandler(stream_handler)

    # Try to attach a rotating file handler if possible
    log_file: Path = config.logs_dir / "magic-ui.log"
    try:
        config.ensure_data_dirs()
        file_handler = RotatingFileHandler(str(log_file), maxBytes=2_000_000, backupCount=3)
        file_handler.setLevel(logging.DEBUG if verbose else logging.INFO)
        file_handler.setFormatter(formatter)
        root_logger.addHandler(file_handler)
        logging.getLogger(__name__).debug("File logging enabled at %s", log_file)
    except Exception as exc:
        logging.getLogger(__name__).warning("File logging unavailable (%s); using console only", exc)

