"""Daemon configuration: command line flags with environment fallbacks."""

from __future__ import annotations

import argparse
import os
from dataclasses import dataclass, field
from pathlib import Path

INSTALL_DIR = Path("/opt/wpilib-websim")


def _env_bool(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")


@dataclass(slots=True)
class Config:
    host: str = "127.0.0.1"
    port: int = 13300
    # Built shell (index.html, assets/) and per-version GUI bundles.
    web_dir: Path = INSTALL_DIR / "web"
    wasm_dir: Path = INSTALL_DIR / "wasm"
    # Shown in the "no simulation running" card.
    projects_dir: str = "~/projects"
    # Disable the robot when the last browser tab goes away.
    auto_disable: bool = True
    disable_grace: float = 3.0
    # Where the robot program listens. Always loopback in a workspace.
    robot_host: str = "127.0.0.1"
    nt_port: int = 5810
    hal_ws_port: int = 3300
    bridge_port: int = 3301
    # Camera ports that may be proxied even if not (yet) advertised in NT.
    camera_ports: range = field(default_factory=lambda: range(1181, 1191))
    # Overrides process-based project detection (tests, non-Linux hosts).
    project: Path | None = None
    log_file: Path | None = None
    log_level: str = "INFO"


def parse_args(argv: list[str] | None = None) -> Config:
    env = os.environ
    p = argparse.ArgumentParser(prog="websimd", description=__doc__)
    p.add_argument("--host", default=env.get("WEBSIMD_HOST", "127.0.0.1"))
    p.add_argument("--port", type=int, default=int(env.get("WEBSIMD_PORT", "13300")))
    p.add_argument(
        "--web-dir", type=Path, default=Path(env.get("WEBSIMD_WEB_DIR", INSTALL_DIR / "web"))
    )
    p.add_argument(
        "--wasm-dir", type=Path, default=Path(env.get("WEBSIMD_WASM_DIR", INSTALL_DIR / "wasm"))
    )
    p.add_argument("--projects-dir", default=env.get("WEBSIMD_PROJECTS_DIR", "~/projects"))
    p.add_argument(
        "--auto-disable",
        action=argparse.BooleanOptionalAction,
        default=_env_bool("WEBSIMD_AUTO_DISABLE", True),
        help="disable the robot shortly after the last browser tab closes",
    )
    p.add_argument(
        "--disable-grace", type=float, default=float(env.get("WEBSIMD_DISABLE_GRACE", "3"))
    )
    p.add_argument("--robot-host", default=env.get("WEBSIMD_ROBOT_HOST", "127.0.0.1"))
    p.add_argument("--nt-port", type=int, default=5810)
    p.add_argument("--hal-ws-port", type=int, default=3300)
    p.add_argument("--bridge-port", type=int, default=3301)
    p.add_argument(
        "--project",
        type=Path,
        default=Path(env["WEBSIMD_PROJECT"]) if env.get("WEBSIMD_PROJECT") else None,
        help="robot project directory; default is to find it from the running robot process",
    )
    p.add_argument(
        "--log-file",
        type=Path,
        default=Path(env["WEBSIMD_LOG_FILE"]) if env.get("WEBSIMD_LOG_FILE") else None,
    )
    p.add_argument("--log-level", default=env.get("WEBSIMD_LOG_LEVEL", "INFO"))
    a = p.parse_args(argv)
    return Config(
        host=a.host,
        port=a.port,
        web_dir=a.web_dir,
        wasm_dir=a.wasm_dir,
        projects_dir=a.projects_dir,
        auto_disable=a.auto_disable,
        disable_grace=a.disable_grace,
        robot_host=a.robot_host,
        nt_port=a.nt_port,
        hal_ws_port=a.hal_ws_port,
        bridge_port=a.bridge_port,
        project=a.project,
        log_file=a.log_file,
        log_level=a.log_level,
    )
