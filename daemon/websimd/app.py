"""HTTP application: routes and background tasks."""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
from collections.abc import AsyncIterator

import aiohttp
from aiohttp import web

from . import __version__
from .camera import CameraProxy
from .config import Config
from .detect import SimDetector
from .files import ProjectFiles
from .halrelay import HalRelay
from .ntwatch import CameraWatcher
from .state import Status
from .static import StaticSite, file_response
from .wsproxy import proxy_websocket

log = logging.getLogger("websimd")

EVENT_KEEPALIVE = 15.0

STATUS = web.AppKey("status", Status)
RELAY = web.AppKey("relay", HalRelay)
SESSION = web.AppKey("session", aiohttp.ClientSession)
CAMERAS = web.AppKey("cameras", CameraProxy)


def create_app(cfg: Config) -> web.Application:
    app = web.Application(client_max_size=64 * 1024 * 1024)
    status = Status(projects_dir=cfg.projects_dir, auto_disable=cfg.auto_disable)
    status.project = cfg.project
    app[STATUS] = status
    files = ProjectFiles(status)
    site = StaticSite(cfg.web_dir, cfg.wasm_dir)

    async def background(app: web.Application) -> AsyncIterator[None]:
        session = aiohttp.ClientSession()
        relay = HalRelay(cfg, status, session)
        cameras = CameraWatcher(cfg, status, session)
        camera_proxy = CameraProxy(cfg, session, lambda: cameras.advertised_ports)
        app[RELAY] = relay
        app[SESSION] = session
        app[CAMERAS] = camera_proxy
        tasks = [
            asyncio.create_task(SimDetector(cfg, status).run(), name="detect"),
            asyncio.create_task(relay.run(), name="hal"),
            asyncio.create_task(cameras.run(), name="nt"),
        ]
        yield
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        await session.close()

    app.cleanup_ctx.append(background)

    # ---------------------------------------------------------------- API

    async def healthz(_: web.Request) -> web.Response:
        return web.json_response({"status": "ok", "version": __version__})

    async def get_status(_: web.Request) -> web.Response:
        return web.json_response(status.to_json(), headers={"Cache-Control": "no-store"})

    async def events(request: web.Request) -> web.StreamResponse:
        """Server-sent events: the status object, on connect and on every change."""
        response = web.StreamResponse(
            headers={
                "Content-Type": "text/event-stream",
                "Cache-Control": "no-store",
                "X-Accel-Buffering": "no",
            }
        )
        await response.prepare(request)
        seen = -1
        with contextlib.suppress(ConnectionError, asyncio.CancelledError):
            while True:
                if status.version != seen:
                    seen = status.version
                    await response.write(f"data: {status.dumps()}\n\n".encode())
                elif not await status.wait_change(seen, EVENT_KEEPALIVE):
                    await response.write(b": keepalive\n\n")
        return response

    async def disable(request: web.Request) -> web.Response:
        sent = await request.app[RELAY].disable_robot()
        return web.json_response({"sent": sent}, status=200 if sent else 409)

    async def fields_index(request: web.Request) -> web.Response:
        bundle = request.query.get("bundle") or status.bundle
        index = cfg.wasm_dir / str(bundle) / "fields" / "index.json"
        if bundle is None or "/" in bundle or not index.is_file():
            return web.json_response([])
        entries = json.loads(index.read_text())
        return web.json_response(
            {"bundle": bundle, "fields": entries}, headers={"Cache-Control": "no-cache"}
        )

    async def fields_file(request: web.Request) -> web.Response:
        bundle = request.query.get("bundle") or status.bundle
        name = request.match_info["file"]
        if bundle is None or "/" in bundle or "/" in name or name.startswith("."):
            raise web.HTTPNotFound()
        path = cfg.wasm_dir / bundle / "fields" / name
        if not path.is_file():
            raise web.HTTPNotFound()
        return file_response(request, path)

    # ------------------------------------------------------------ proxies

    async def nt(request: web.Request) -> web.StreamResponse:
        client_id = request.match_info["client_id"]
        url = f"ws://{cfg.robot_host}:{cfg.nt_port}/nt/{client_id}"
        return await proxy_websocket(request, request.app[SESSION], url)

    async def hal(request: web.Request) -> web.StreamResponse:
        return await request.app[RELAY].handle(request)

    async def cam(request: web.Request) -> web.StreamResponse:
        return await request.app[CAMERAS].handle(request)

    r = app.router
    r.add_get("/healthz", healthz)
    r.add_get("/api/status", get_status)
    r.add_get("/api/events", events)
    r.add_post("/api/sim/disable", disable)
    r.add_get("/api/storage/{name}", files.get_storage)
    r.add_put("/api/storage/{name}", files.put_storage)
    r.add_get("/api/fs", files.list_dir)
    r.add_get("/api/fs/file", files.get_file)
    r.add_post("/api/fs/upload", files.upload)
    r.add_get("/api/fields", fields_index)
    r.add_get("/api/fields/{file}", fields_file)
    r.add_get("/nt/{client_id}", nt)
    r.add_get("/hal", hal)
    r.add_get("/cam/{port}/stream.mjpg", cam)
    r.add_get("/wasm/{path:.+}", site.wasm)
    r.add_route("*", "/api/{rest:.*}", _not_found)
    r.add_get("/{path:.*}", site.site)
    return app


async def _not_found(_: web.Request) -> web.Response:
    raise web.HTTPNotFound()
