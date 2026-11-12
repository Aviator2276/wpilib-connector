"""MJPEG proxy for cscore camera servers (`/cam/<port>/stream.mjpg`).

Chrome decodes ``multipart/x-mixed-replace`` natively in an ``<img>``, so the
daemon only pipes bytes. A stream is open upstream exactly as long as a
browser is reading it: when the image is unmounted the request is cancelled
here, the upstream connection closes, and cscore stops encoding for it.
"""

from __future__ import annotations

import logging
from collections.abc import Callable, Collection

import aiohttp
from aiohttp import web

from .config import Config

log = logging.getLogger("websimd.camera")


class CameraProxy:
    def __init__(
        self,
        cfg: Config,
        session: aiohttp.ClientSession,
        advertised: Callable[[], Collection[int]],
    ) -> None:
        self.cfg = cfg
        self.session = session
        self.advertised = advertised

    def allowed(self, port: int) -> bool:
        return port in self.cfg.camera_ports or port in self.advertised()

    async def handle(self, request: web.Request) -> web.StreamResponse:
        try:
            port = int(request.match_info["port"])
        except ValueError:
            raise web.HTTPBadRequest(text="bad port") from None
        if not self.allowed(port):
            raise web.HTTPForbidden(text="not a camera port")
        url = f"http://{self.cfg.robot_host}:{port}/stream.mjpg"
        timeout = aiohttp.ClientTimeout(total=None, sock_connect=3, sock_read=15)
        try:
            upstream = await self.session.get(url, timeout=timeout)
        except (aiohttp.ClientError, OSError, TimeoutError) as e:
            raise web.HTTPBadGateway(text="camera is not reachable") from e
        try:
            if upstream.status != 200:
                raise web.HTTPBadGateway(text=f"camera answered {upstream.status}")
            response = web.StreamResponse(
                headers={
                    "Content-Type": upstream.headers.get(
                        "Content-Type", "multipart/x-mixed-replace;boundary=boundarydonotcross"
                    ),
                    "Cache-Control": "no-store, no-cache, must-revalidate",
                    "Pragma": "no-cache",
                    # ask reverse proxies not to buffer the endless response
                    "X-Accel-Buffering": "no",
                }
            )
            await response.prepare(request)
            async for chunk in upstream.content.iter_any():
                await response.write(chunk)
            return response
        except (ConnectionError, aiohttp.ClientError, TimeoutError):
            return web.Response(status=499)  # browser or camera went away mid-stream
        finally:
            upstream.close()
