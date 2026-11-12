"""The daemon's view of the simulation, published to browsers.

One mutable `Status` object is the source for `GET /api/status` and for the
`/api/events` stream. Anything that changes it calls `changed()`.
"""

from __future__ import annotations

import asyncio
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass(slots=True)
class Camera:
    name: str
    port: int


@dataclass(slots=True)
class Status:
    # robot program
    running: bool = False
    pid: int | None = None
    project: Path | None = None
    wpilib_version: str | None = None
    bundle: str | None = None
    started_at: str | None = None
    # links to it
    nt_up: bool = False
    hal_up: bool = False
    hal_mode: str | None = None  # "ws" (stock halsim_ws) or "bridge"
    cameras: list[Camera] = field(default_factory=list)
    clients: int = 0
    # static facts the shell shows
    projects_dir: str = "~/projects"
    auto_disable: bool = True
    bundles: list[str] = field(default_factory=list)

    _version: int = 0
    _waiters: set[asyncio.Event] = field(default_factory=set)

    def to_json(self) -> dict[str, Any]:
        return {
            "sim": {
                "running": self.running,
                "pid": self.pid,
                "project": str(self.project) if self.project else None,
                "wpilibVersion": self.wpilib_version,
                "bundle": self.bundle,
                "startedAt": self.started_at,
            },
            "nt": {"up": self.nt_up},
            "hal": {"up": self.hal_up, "mode": self.hal_mode},
            "cameras": [{"name": c.name, "port": c.port} for c in self.cameras],
            "clients": self.clients,
            "config": {
                "projectsDir": self.projects_dir,
                "autoDisable": self.auto_disable,
                "bundles": self.bundles,
            },
        }

    def dumps(self) -> str:
        return json.dumps(self.to_json(), separators=(",", ":"))

    @property
    def version(self) -> int:
        return self._version

    def changed(self) -> None:
        self._version += 1
        for ev in self._waiters:
            ev.set()

    async def wait_change(self, seen_version: int, timeout: float) -> bool:
        """Wait until the status differs from `seen_version`; False on timeout."""
        if self._version != seen_version:
            return True
        ev = asyncio.Event()
        self._waiters.add(ev)
        try:
            await asyncio.wait_for(ev.wait(), timeout)
            return True
        except TimeoutError:
            return False
        finally:
            self._waiters.discard(ev)
