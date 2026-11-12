"""A minimal NT4 subscriber that discovers camera streams.

cscore publishes each camera under ``/CameraPublisher/<name>/`` with a
``streams`` string array such as ``["mjpg:http://127.0.0.1:1181/?action=stream"]``.
The daemon lists those so the shell can show cameras and so ``/cam/<port>``
only proxies ports that really are camera servers.

Only what that needs is implemented: one prefix subscription, topic
announcements (JSON text frames) and value updates (MessagePack binary
frames of ``[topic id, timestamp, type, value]``).
"""

from __future__ import annotations

import asyncio
import json
import logging
import re
from urllib.parse import urlsplit

import aiohttp
import msgpack
from aiohttp import WSMsgType

from .config import Config
from .state import Camera, Status

log = logging.getLogger("websimd.nt")

PREFIX = "/CameraPublisher/"
_STREAMS = re.compile(r"^/CameraPublisher/(.+)/streams$")
SUBPROTOCOLS = ("v4.1.networktables.first.wpi.edu", "networktables.first.wpi.edu")


def stream_port(streams: list[str]) -> int | None:
    """Port of the first HTTP MJPEG stream in a cscore ``streams`` value."""
    for s in streams:
        if not isinstance(s, str):
            continue
        url = s.split(":", 1)[1] if s.startswith("mjpg:") else s
        try:
            parts = urlsplit(url)
            port = parts.port
        except ValueError:
            continue
        if parts.scheme == "http" and port:
            return port
    return None


class CameraWatcher:
    def __init__(self, cfg: Config, status: Status, session: aiohttp.ClientSession) -> None:
        self.cfg = cfg
        self.status = status
        self.session = session
        self._topics: dict[int, str] = {}  # topic id -> camera name
        self._ports: dict[str, int] = {}  # camera name -> port

    @property
    def advertised_ports(self) -> set[int]:
        return set(self._ports.values())

    def _publish(self) -> None:
        cams = [Camera(name, port) for name, port in sorted(self._ports.items())]
        if cams != self.status.cameras:
            self.status.cameras = cams
            self.status.changed()

    def _set_up(self, up: bool) -> None:
        if self.status.nt_up != up:
            self.status.nt_up = up
            self.status.changed()

    async def run(self) -> None:
        url = f"ws://{self.cfg.robot_host}:{self.cfg.nt_port}/nt/websimd"
        while True:
            if self.status.running:
                try:
                    await self._session(url)
                except (aiohttp.ClientError, OSError, TimeoutError) as e:
                    log.debug("NT: %s", e)
                except Exception:
                    log.exception("NT watcher failed")
                self._topics.clear()
                self._ports.clear()
                self._publish()
                self._set_up(False)
            await asyncio.sleep(1.0)

    async def _session(self, url: str) -> None:
        async with self.session.ws_connect(
            url, protocols=SUBPROTOCOLS, max_msg_size=0, heartbeat=2.0
        ) as ws:
            self._set_up(True)
            await ws.send_str(
                json.dumps(
                    [
                        {
                            "method": "subscribe",
                            "params": {
                                "topics": [PREFIX],
                                "subuid": 1,
                                "options": {"prefix": True},
                            },
                        }
                    ]
                )
            )
            unpacker = msgpack.Unpacker(raw=False, strict_map_key=False)
            async for msg in ws:
                if msg.type == WSMsgType.TEXT:
                    self._on_text(msg.data)
                elif msg.type == WSMsgType.BINARY:
                    unpacker.feed(msg.data)
                    for item in unpacker:
                        self._on_value(item)
                else:
                    break

    def _on_text(self, text: str) -> None:
        try:
            messages = json.loads(text)
        except ValueError:
            return
        for m in messages if isinstance(messages, list) else []:
            if not isinstance(m, dict):
                continue
            params = m.get("params") or {}
            method = m.get("method")
            if method == "announce":
                match = _STREAMS.match(str(params.get("name", "")))
                if match and isinstance(params.get("id"), int):
                    self._topics[params["id"]] = match.group(1)
            elif method == "unannounce":
                name = self._topics.pop(params.get("id"), None)
                if name and self._ports.pop(name, None) is not None:
                    self._publish()

    def _on_value(self, item: object) -> None:
        if not (isinstance(item, list) and len(item) == 4):
            return
        topic_id, _timestamp, _type, value = item
        name = self._topics.get(topic_id) if isinstance(topic_id, int) else None
        if name is None or not isinstance(value, list):
            return
        port = stream_port(value)
        if port is None:
            if self._ports.pop(name, None) is not None:
                self._publish()
        elif self._ports.get(name) != port:
            self._ports[name] = port
            self._publish()
