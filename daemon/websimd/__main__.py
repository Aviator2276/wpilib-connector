"""Entry point: ``python -m websimd`` / ``websimd``."""

from __future__ import annotations

import asyncio
import logging
import logging.handlers
import sys

from aiohttp import web

from . import __version__
from .app import create_app
from .config import Config, parse_args


def setup_logging(cfg: Config) -> None:
    handlers: list[logging.Handler] = [logging.StreamHandler(sys.stderr)]
    if cfg.log_file is not None:
        cfg.log_file.parent.mkdir(parents=True, exist_ok=True)
        handlers.append(
            logging.handlers.RotatingFileHandler(cfg.log_file, maxBytes=1_000_000, backupCount=2)
        )
    logging.basicConfig(
        level=getattr(logging, cfg.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        handlers=handlers,
    )
    # one line per proxied request is noise at camera/asset volume
    logging.getLogger("aiohttp.access").setLevel(logging.WARNING)


def main(argv: list[str] | None = None) -> None:
    cfg = parse_args(argv)
    setup_logging(cfg)
    try:
        import uvloop

        asyncio.set_event_loop_policy(uvloop.EventLoopPolicy())
    except ImportError:
        pass
    logging.getLogger("websimd").info(
        "websimd %s on http://%s:%d (web=%s wasm=%s auto_disable=%s)",
        __version__,
        cfg.host,
        cfg.port,
        cfg.web_dir,
        cfg.wasm_dir,
        cfg.auto_disable,
    )
    web.run_app(create_app(cfg), host=cfg.host, port=cfg.port, print=None, access_log=None)


if __name__ == "__main__":
    main()
