"""Stand-ins for the robot program's network endpoints."""

from __future__ import annotations

import json

import msgpack
from aiohttp import WSMsgType, web


class FakeHalWs:
    """halsim_ws_server: JSON, and only one client at a time (HTTP 409 otherwise)."""

    def __init__(self, initial: list[dict]) -> None:
        self.initial = initial
        self.received: list[dict] = []
        self.ws: web.WebSocketResponse | None = None
        self.connections = 0

    async def handler(self, request: web.Request) -> web.StreamResponse:
        if self.ws is not None and not self.ws.closed:
            return web.Response(status=409, text="Only a single simulation websocket is allowed")
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        self.ws = ws
        self.connections += 1
        for msg in self.initial:
            await ws.send_str(json.dumps(msg))
        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                self.received.append(json.loads(msg.data))
        return ws

    async def send(self, type_: str, device: str, data: dict) -> None:
        assert self.ws is not None
        await self.ws.send_str(json.dumps({"type": type_, "device": device, "data": data}))

    def app(self) -> web.Application:
        app = web.Application()
        app.router.add_get("/wpilibws", self.handler)
        return app


class FakeNt:
    """NT4 server: echoes binary frames, records text, announces a camera."""

    def __init__(self, streams: list[str] | None = None) -> None:
        self.streams = streams
        self.protocols: list[str | None] = []
        self.paths: list[str] = []

    async def handler(self, request: web.Request) -> web.StreamResponse:
        ws = web.WebSocketResponse(
            protocols=("v4.1.networktables.first.wpi.edu", "networktables.first.wpi.edu")
        )
        await ws.prepare(request)
        self.protocols.append(ws.ws_protocol)
        self.paths.append(request.path)
        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                body = json.loads(msg.data)
                if self.streams is not None and body and body[0].get("method") == "subscribe":
                    await ws.send_str(
                        json.dumps(
                            [
                                {
                                    "method": "announce",
                                    "params": {
                                        "name": "/CameraPublisher/frontcam-raw/streams",
                                        "id": 7,
                                        "type": "string[]",
                                        "properties": {},
                                    },
                                }
                            ]
                        )
                    )
                    await ws.send_bytes(msgpack.packb([7, 1000, 20, self.streams]))
                else:
                    await ws.send_str(msg.data)
            elif msg.type == WSMsgType.BINARY:
                await ws.send_bytes(msg.data)
        return ws

    def app(self) -> web.Application:
        app = web.Application()
        app.router.add_get("/nt/{id}", self.handler)
        return app
