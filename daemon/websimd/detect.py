"""Detect the running robot program and which GUI bundle it needs."""

from __future__ import annotations

import asyncio
import logging
import re
from datetime import UTC, datetime
from pathlib import Path

from . import procfs
from .config import Config
from .state import Status

log = logging.getLogger("websimd.detect")

_GRADLERIO = re.compile(
    r"""(?:edu\.wpi\.first|org\.wpilib)\.GradleRIO["')]*\s*\)?\s*version\s*\(?\s*["']([^"']+)["']"""
)
_VERSION_KEY = re.compile(r"\d+")


async def port_open(host: str, port: int, timeout: float = 0.5) -> bool:
    try:
        _, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout)
    except (OSError, TimeoutError):
        return False
    writer.close()
    try:
        await writer.wait_closed()
    except OSError:
        pass
    return True


async def is_listening(host: str, port: int) -> bool:
    """Whether the robot program has `port` open, without connecting to it.

    The robot's NetworkTables server logs every connection, so a probe that
    connects once a second floods the student's console with
    "Got a NT4 connection" lines. On Linux, for the loopback robot, the
    kernel's socket table answers the question silently. Anywhere else (tests,
    a robot on another host) there is nothing better than a connection.
    """
    if host in ("127.0.0.1", "localhost", "::1"):
        listening = procfs.port_listening(port)
        if listening is not None:
            return listening
    return await port_open(host, port)


def is_robot_project(path: Path) -> bool:
    return (path / ".wpilib" / "wpilib_preferences.json").is_file()


def project_wpilib_version(project: Path) -> str | None:
    """The GradleRIO plugin version declared by the project's build file."""
    for name in ("build.gradle", "build.gradle.kts"):
        try:
            text = (project / name).read_text(errors="replace")
        except OSError:
            continue
        if m := _GRADLERIO.search(text):
            return m.group(1)
    return None


def _version_key(version: str) -> tuple[int, ...]:
    return tuple(int(n) for n in _VERSION_KEY.findall(version))


def list_bundles(wasm_dir: Path) -> list[str]:
    """Installed GUI bundle versions, newest first."""
    try:
        names = [d.name for d in wasm_dir.iterdir() if d.is_dir() and (d / "simgui.wasm").is_file()]
    except OSError:
        return []
    return sorted(names, key=_version_key, reverse=True)


def choose_bundle(bundles: list[str], wpilib_version: str | None) -> str | None:
    """Exact version, else the newest bundle of the same season, else the newest."""
    if not bundles:
        return None
    if wpilib_version:
        if wpilib_version in bundles:
            return wpilib_version
        season = wpilib_version.split(".", 1)[0]
        for b in bundles:
            if b.split(".", 1)[0] == season:
                return b
    return bundles[0]


class SimDetector:
    """Looks for the robot's NetworkTables server once a second."""

    def __init__(self, cfg: Config, status: Status) -> None:
        self.cfg = cfg
        self.status = status

    def _locate(self) -> tuple[int | None, Path | None]:
        if self.cfg.project is not None:
            return None, self.cfg.project
        pid = procfs.pid_listening_on(self.cfg.nt_port)
        if pid is None:
            return None, None
        cwd = procfs.process_cwd(pid)
        if cwd is not None and not is_robot_project(cwd):
            log.info("NT server pid %s has cwd %s, which is not a robot project", pid, cwd)
            cwd = None
        return pid, cwd

    async def poll_once(self) -> None:
        st = self.status
        up = await is_listening(self.cfg.robot_host, self.cfg.nt_port)
        if up:
            pid, project = await asyncio.to_thread(self._locate)
            if not st.running or pid != st.pid or project != st.project:
                version = project_wpilib_version(project) if project else None
                started = procfs.process_start_time(pid) if pid else None
                st.running = True
                st.pid = pid
                st.project = project
                st.wpilib_version = version
                st.bundles = list_bundles(self.cfg.wasm_dir)
                st.bundle = choose_bundle(st.bundles, version)
                st.started_at = (
                    datetime.fromtimestamp(started, UTC) if started else datetime.now(UTC)
                ).strftime("%Y-%m-%dT%H:%M:%SZ")
                log.info(
                    "robot program found: pid=%s project=%s wpilib=%s bundle=%s",
                    pid,
                    project,
                    version,
                    st.bundle,
                )
                st.changed()
        elif st.running:
            log.info("robot program gone")
            st.running = False
            st.pid = None
            st.started_at = None
            st.changed()

    async def run(self) -> None:
        self.status.bundles = list_bundles(self.cfg.wasm_dir)
        self.status.bundle = choose_bundle(self.status.bundles, None)
        while True:
            try:
                await self.poll_once()
            except Exception:
                log.exception("detection failed")
            await asyncio.sleep(1.0)
