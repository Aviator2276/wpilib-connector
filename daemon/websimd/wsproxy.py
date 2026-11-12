"""Transparent WebSocket proxy: one upstream connection per browser socket.

Used for NetworkTables (`/nt/<id>` to the robot's NT4 server) and for the
native HAL bridge, both of which accept any number of clients themselves.
"""

from __future__ import annotations

import asyncio
import logging

import aiohttp
from aiohttp import WSMsgType, web

log = logging.getLogger("websimd.wsproxy")

# NT4 values (camera frames aside) are small, but schemas and large arrays
# can be big; do not impose a limit the robot does not have.
MAX_MSG = 0


async def proxy_websocket(
    request: web.Request,
    session: aiohttp.ClientSession,
    upstream_url: str,
    *,
    compress: bool = False,
) -> web.StreamResponse:
    """Pipe `request`'s WebSocket to `upstream_url` until either side closes.

    The subprotocols the browser offers are offered upstream unchanged and the
    one upstream selects is echoed back, so NT4 version negotiation happens
    between the real endpoints.
    """
    offered = [
        p.strip() for p in request.headers.get("Sec-WebSocket-Protocol", "").split(",") if p.strip()
    ]
    try:
        upstream = await session.ws_connect(
            upstream_url,
            protocols=offered,
            max_msg_size=MAX_MSG,
            # Browsers cannot send WebSocket pings, so liveness of the robot
            # end is checked here instead; a dead robot closes the browser
            # socket and the client reconnects.
            heartbeat=2.0,
            autoping=True,
            compress=0,
            timeout=aiohttp.ClientWSTimeout(ws_close=2.0),
        )
    except (aiohttp.ClientError, OSError, TimeoutError) as e:
        log.debug("upstream %s unavailable: %s", upstream_url, e)
        raise web.HTTPBadGateway(text="robot program is not reachable") from e

    protocols = (upstream.protocol,) if upstream.protocol else ()
    ws = web.WebSocketResponse(protocols=protocols, max_msg_size=MAX_MSG, compress=compress)
    await ws.prepare(request)

    async def pump_down() -> None:  # robot -> browser
        async for msg in upstream:
            if msg.type == WSMsgType.BINARY:
                await ws.send_bytes(msg.data)
            elif msg.type == WSMsgType.TEXT:
                await ws.send_str(msg.data)
            else:
                break

    async def pump_up() -> None:  # browser -> robot
        async for msg in ws:
            if msg.type == WSMsgType.BINARY:
                await upstream.send_bytes(msg.data)
            elif msg.type == WSMsgType.TEXT:
                await upstream.send_str(msg.data)
            else:
                break

    tasks = [asyncio.create_task(pump_down()), asyncio.create_task(pump_up())]
    try:
        await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        await upstream.close()
        await ws.close()
    return ws
