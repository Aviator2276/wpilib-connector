"""HAL relay between browser tabs (`/hal`) and the robot program.

Two upstream modes, chosen by what the robot process offers:

``bridge``
    The native halsim_webbridge extension (binary protocol, many clients).
    Each tab gets its own proxied connection; the bridge does its own
    snapshots and coalescing. The daemon keeps one control connection for
    the auto-disable rule.

``ws``
    Stock halsim_ws_server (JSON, exactly one client allowed). The daemon
    holds that single connection and fans out:

    * a cache of the latest value for every (type, device, key), so a tab
      that connects late first receives a full snapshot;
    * robot-to-browser changes merged per key and flushed as one JSON array
      every ``FLUSH_INTERVAL`` seconds (Phoenix 6 devices alone emit about
      1,100 messages a second, mostly supply-voltage noise);
    * browser-to-robot messages forwarded immediately and echoed to the
      other tabs so they agree.

Safety: the robot stays enabled when a browser goes away (unlike the desktop
GUI, whose window owns the robot process). When the last tab disconnects and
none returns within the grace period, the relay disables the robot.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
from typing import Any

import aiohttp
from aiohttp import WSMsgType, web

from .config import Config
from .detect import is_listening
from .state import Status
from .wsproxy import proxy_websocket

try:  # optional accelerator
    import orjson

    def _loads(s: str | bytes) -> Any:
        return orjson.loads(s)

    def _dumps(o: Any) -> str:
        return orjson.dumps(o).decode()

except ImportError:  # pragma: no cover - exercised when orjson is absent
    _loads = json.loads

    def _dumps(o: Any) -> str:
        return json.dumps(o, separators=(",", ":"))


log = logging.getLogger("websimd.hal")

FLUSH_INTERVAL = 0.033
SNAPSHOT_CHUNK = 256
# One-shot keys: events, not state. Never cached or replayed to late tabs.
ONE_SHOT_KEYS = frozenset({">new_data", ">sim_periodic_before", ">sim_periodic_after"})

DISABLE_MESSAGES = (
    {"type": "DriverStation", "device": "", "data": {">enabled": False}},
    {"type": "DriverStation", "device": "", "data": {">new_data": True}},
)

Key = tuple[str, str]


def valid_message(msg: Any) -> bool:
    return (
        isinstance(msg, dict)
        and isinstance(msg.get("type"), str)
        and isinstance(msg.get("device"), str)
        and isinstance(msg.get("data"), dict)
    )


class HalCache:
    """Latest value per (type, device, key) plus the set changed since a flush."""

    def __init__(self) -> None:
        self.state: dict[Key, dict[str, Any]] = {}
        self.dirty: dict[Key, dict[str, Any]] = {}

    def clear(self) -> None:
        self.state.clear()
        self.dirty.clear()

    def update(self, msg: dict[str, Any], *, mark_dirty: bool = True) -> None:
        key = (msg["type"], msg["device"])
        data = msg["data"]
        cached = self.state.get(key)
        if cached is None:
            cached = self.state[key] = {}
        pending = None
        for k, v in data.items():
            if k in ONE_SHOT_KEYS:
                continue
            if mark_dirty and cached.get(k, cached) != v:
                if pending is None:
                    pending = self.dirty.get(key)
                    if pending is None:
                        pending = self.dirty[key] = {}
                pending[k] = v
            cached[k] = v

    def take_dirty(self) -> list[dict[str, Any]]:
        if not self.dirty:
            return []
        out = [{"type": t, "device": d, "data": data} for (t, d), data in self.dirty.items()]
        self.dirty = {}
        return out

    def snapshot(self) -> list[dict[str, Any]]:
        return [
            {"type": t, "device": d, "data": data} for (t, d), data in self.state.items() if data
        ]


class HalRelay:
    def __init__(self, cfg: Config, status: Status, session: aiohttp.ClientSession) -> None:
        self.cfg = cfg
        self.status = status
        self.session = session
        self.cache = HalCache()
        self.clients: set[web.WebSocketResponse] = set()
        self._tabs = 0
        self._upstream: aiohttp.ClientWebSocketResponse | None = None
        self._disable_timer: asyncio.TimerHandle | None = None
        self._tasks: set[asyncio.Task[None]] = set()

    # ------------------------------------------------------------- upstream

    @property
    def _ws_url(self) -> str:
        return f"ws://{self.cfg.robot_host}:{self.cfg.hal_ws_port}/wpilibws"

    @property
    def _bridge_url(self) -> str:
        return f"ws://{self.cfg.robot_host}:{self.cfg.bridge_port}/"

    def _set_up(self, up: bool, mode: str | None) -> None:
        st = self.status
        if st.hal_up != up or st.hal_mode != mode:
            st.hal_up, st.hal_mode = up, mode
            st.changed()

    async def run(self) -> None:
        """Keep a connection to whichever HAL endpoint the robot offers."""
        flusher = asyncio.create_task(self._flush_loop())
        try:
            while True:
                try:
                    if not self.status.running:
                        pass
                    elif await is_listening(self.cfg.robot_host, self.cfg.bridge_port):
                        # The bridge sends no HAL data on its control path.
                        await self._run_upstream(self._bridge_url + "control", "bridge")
                    elif await is_listening(self.cfg.robot_host, self.cfg.hal_ws_port):
                        await self._run_upstream(self._ws_url, "ws")
                except (aiohttp.ClientError, OSError, TimeoutError) as e:
                    log.debug("HAL upstream: %s", e)
                except Exception:
                    log.exception("HAL upstream failed")
                await asyncio.sleep(1.0)
        finally:
            flusher.cancel()

    async def _run_upstream(self, url: str, mode: str) -> None:
        async with self.session.ws_connect(
            url, max_msg_size=0, heartbeat=2.0, timeout=aiohttp.ClientWSTimeout(ws_close=2.0)
        ) as upstream:
            log.info("HAL upstream connected (%s)", mode)
            self.cache.clear()
            self._upstream = upstream
            self._set_up(True, mode)
            try:
                async for msg in upstream:
                    if msg.type == WSMsgType.TEXT:
                        if mode == "ws":
                            self._on_robot_text(msg.data)
                    elif msg.type != WSMsgType.BINARY:
                        break
            finally:
                self._upstream = None
                self.cache.clear()
                self._set_up(False, None)
                log.info("HAL upstream closed")
                # Tabs reconnect and get a fresh snapshot (and possibly the
                # other mode) on the robot's next run.
                for ws in list(self.clients):
                    with contextlib.suppress(Exception):
                        await ws.close(code=1012, message=b"robot program ended")

    def _on_robot_text(self, text: str) -> None:
        try:
            msg = _loads(text)
        except ValueError:
            return
        if valid_message(msg):
            self.cache.update(msg)

    async def _flush_loop(self) -> None:
        while True:
            await asyncio.sleep(FLUSH_INTERVAL)
            if not self.cache.dirty:
                continue
            batch = self.cache.take_dirty()
            if self.clients:
                await self._broadcast(_dumps(batch), exclude=None)

    async def _broadcast(self, payload: str, exclude: web.WebSocketResponse | None) -> None:
        for ws in list(self.clients):
            if ws is exclude or ws.closed:
                continue
            try:
                await ws.send_str(payload)
            except (ConnectionError, RuntimeError):
                self.clients.discard(ws)

    # ----------------------------------------------------------- downstream

    async def handle(self, request: web.Request) -> web.StreamResponse:
        mode = self.status.hal_mode
        if mode is None:
            # Nothing to talk to yet; the mirror retries every second.
            raise web.HTTPServiceUnavailable(text="robot program has no HAL endpoint")
        self._tab_opened()
        try:
            if mode == "bridge":
                return await proxy_websocket(request, self.session, self._bridge_url, compress=True)
            return await self._handle_ws_client(request)
        finally:
            self._tab_closed()

    async def _handle_ws_client(self, request: web.Request) -> web.StreamResponse:
        ws = web.WebSocketResponse(max_msg_size=4 * 1024 * 1024, compress=True)
        await ws.prepare(request)
        snapshot = self.cache.snapshot()
        for i in range(0, len(snapshot), SNAPSHOT_CHUNK):
            await ws.send_str(_dumps(snapshot[i : i + SNAPSHOT_CHUNK]))
        self.clients.add(ws)
        try:
            async for msg in ws:
                if msg.type == WSMsgType.TEXT:
                    await self._on_browser_text(ws, msg.data)
                elif msg.type != WSMsgType.BINARY:
                    break
        finally:
            self.clients.discard(ws)
        return ws

    async def _on_browser_text(self, sender: web.WebSocketResponse, text: str) -> None:
        try:
            frame = _loads(text)
        except ValueError:
            return
        msgs = [m for m in (frame if isinstance(frame, list) else [frame]) if valid_message(m)]
        if not msgs:
            return
        upstream = self._upstream
        if upstream is not None and not upstream.closed:
            # halsim_ws_server takes exactly one message object per frame.
            for m in msgs:
                await upstream.send_str(_dumps(m))
        echo = []
        for m in msgs:
            # Joystick input is per-tab: each GUI produces its own and would
            # only overwrite a copy of another tab's.
            if m["type"] == "Joystick":
                continue
            self.cache.update(m, mark_dirty=False)
            data = {k: v for k, v in m["data"].items() if k not in ONE_SHOT_KEYS}
            if data:
                echo.append({"type": m["type"], "device": m["device"], "data": data})
        if echo and len(self.clients) > 1:
            await self._broadcast(_dumps(echo), exclude=sender)

    # --------------------------------------------------------------- safety

    def _tab_opened(self) -> None:
        self._tabs += 1
        if self._disable_timer is not None:
            self._disable_timer.cancel()
            self._disable_timer = None
        self._publish_tabs()

    def _tab_closed(self) -> None:
        self._tabs = max(0, self._tabs - 1)
        self._publish_tabs()
        if self._tabs == 0 and self.cfg.auto_disable:
            loop = asyncio.get_running_loop()
            self._disable_timer = loop.call_later(self.cfg.disable_grace, self._grace_expired)

    def _publish_tabs(self) -> None:
        if self.status.clients != self._tabs:
            self.status.clients = self._tabs
            self.status.changed()

    def _grace_expired(self) -> None:
        self._disable_timer = None
        if self._tabs == 0 and self.status.running:
            log.info("last browser tab closed; disabling the robot")
            task = asyncio.get_running_loop().create_task(self.disable_robot())
            self._tasks.add(task)
            task.add_done_callback(self._tasks.discard)

    async def disable_robot(self) -> bool:
        """Force the robot program to disabled. True if the request was sent."""
        upstream = self._upstream
        if upstream is None or upstream.closed:
            return False
        try:
            if self.status.hal_mode == "bridge":
                await upstream.send_str('{"cmd":"disable"}')
            else:
                for m in DISABLE_MESSAGES:
                    await upstream.send_str(_dumps(m))
                self.cache.update(DISABLE_MESSAGES[0])
        except (ConnectionError, RuntimeError):
            return False
        return True
