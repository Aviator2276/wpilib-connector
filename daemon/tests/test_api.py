"""Static hosting, status/events, NT proxy, camera discovery and proxy."""

from __future__ import annotations

import asyncio
import json

import msgpack
from aiohttp import web
from fake_robot import FakeNt
from test_halrelay import wait_for

from websimd.app import STATUS, create_app
from websimd.detect import choose_bundle, list_bundles, project_wpilib_version
from websimd.ntwatch import stream_port


async def test_healthz_and_status_shape(cfg, aiohttp_client):
    client = await aiohttp_client(create_app(cfg))
    resp = await client.get("/healthz")
    assert resp.status == 200
    assert (await resp.json())["status"] == "ok"
    status = await (await client.get("/api/status")).json()
    assert status["sim"]["running"] is False
    assert status["hal"] == {"up": False, "mode": None}
    assert status["cameras"] == []
    assert status["config"]["bundles"] == ["2026.2.1", "2026.1.1"]
    assert status["sim"]["bundle"] == "2026.2.1"


async def test_events_stream_sends_current_state_then_changes(cfg, aiohttp_client):
    client = await aiohttp_client(create_app(cfg))
    resp = await client.get("/api/events")
    assert resp.headers["Content-Type"] == "text/event-stream"
    first = await resp.content.readuntil(b"\n\n")
    assert json.loads(first.removeprefix(b"data: "))["nt"]["up"] is False
    status = client.app[STATUS]
    status.nt_up = True
    status.changed()
    second = await asyncio.wait_for(resp.content.readuntil(b"\n\n"), 2)
    assert json.loads(second.removeprefix(b"data: "))["nt"]["up"] is True
    resp.close()


def test_bundle_selection(site, project):
    _, wasm = site
    bundles = list_bundles(wasm)
    assert bundles == ["2026.2.1", "2026.1.1"]
    assert project_wpilib_version(project) == "2026.2.1"
    assert choose_bundle(bundles, "2026.1.1") == "2026.1.1"
    assert choose_bundle(bundles, "2026.9.9") == "2026.2.1"  # same season, newest
    assert choose_bundle(bundles, "2031.1.1") == "2026.2.1"  # unknown season, newest
    assert choose_bundle(bundles, None) == "2026.2.1"
    assert choose_bundle([], "2026.2.1") is None


def test_gradlerio_version_forms(tmp_path):
    for text in (
        "id 'edu.wpi.first.GradleRIO' version '2026.2.1'",
        'id("edu.wpi.first.GradleRIO") version "2026.2.1"',
        'id "org.wpilib.GradleRIO" version "2026.2.1"',
    ):
        (tmp_path / "build.gradle").write_text(f"plugins {{\n  {text}\n}}\n")
        assert project_wpilib_version(tmp_path) == "2026.2.1", text
    (tmp_path / "build.gradle").write_text("plugins { id 'java' }")
    assert project_wpilib_version(tmp_path) is None


async def test_static_site(cfg, aiohttp_client):
    client = await aiohttp_client(create_app(cfg))
    index = await client.get("/")
    assert index.headers["Cache-Control"] == "no-cache"
    assert '<base href="./">' in await index.text()
    # client-side routes fall back to the shell, with a base that still points
    # at the app root (the app also runs under Coder's path prefix)
    assert '<base href="./">' in await (await client.get("/cameras")).text()
    assert '<base href="../">' in await (await client.get("/a/b")).text()
    asset = await client.get("/assets/app-abc123.js")
    assert "immutable" in asset.headers["Cache-Control"]
    assert (await client.get("/assets/missing.js")).status == 404
    assert (await client.get("/api/nope")).status == 404
    # traversal attempts get the shell or a 404, never a file outside web_dir
    escaped = await client.get("/..%2f..%2fetc/passwd")
    assert escaped.status == 404 or "<title>t</title>" in await escaped.text()


async def test_wasm_bundle_is_served_precompressed(cfg, aiohttp_client):
    client = await aiohttp_client(create_app(cfg), auto_decompress=False)
    resp = await client.get("/wasm/2026.2.1/simgui.wasm", headers={"Accept-Encoding": "gzip, br"})
    assert resp.headers["Content-Type"] == "application/wasm"
    assert resp.headers["Content-Encoding"] == "br"
    assert await resp.read() == b"brotli2026.2.1"
    plain = await client.get("/wasm/2026.2.1/simgui.wasm", headers={"Accept-Encoding": "identity"})
    assert "Content-Encoding" not in plain.headers
    assert await plain.read() == b"\0asm2026.2.1"
    again = await client.get(
        "/wasm/2026.2.1/simgui.wasm", headers={"If-None-Match": plain.headers["ETag"]}
    )
    assert again.status == 304
    assert (await client.get("/wasm/../web/index.html")).status == 404


async def test_fields(cfg, aiohttp_client):
    client = await aiohttp_client(create_app(cfg))
    body = await (await client.get("/api/fields")).json()
    assert body["bundle"] == "2026.2.1"
    assert body["fields"][0]["image"] == "2026-field.png"
    assert (await client.get("/api/fields/2026-rebuilt.json")).status == 200
    assert (await client.get("/api/fields/index.json?bundle=../x")).status == 404


async def test_nt_proxy_passes_subprotocol_and_frames(cfg, aiohttp_server, aiohttp_client):
    nt = FakeNt()
    cfg.nt_port = (await aiohttp_server(nt.app())).port
    client = await aiohttp_client(create_app(cfg))
    ws = await client.ws_connect(
        "/nt/websim",
        protocols=("v4.1.networktables.first.wpi.edu", "networktables.first.wpi.edu"),
    )
    assert ws.protocol == "v4.1.networktables.first.wpi.edu"
    await ws.send_str('[{"method":"publish"}]')
    assert await ws.receive_str() == '[{"method":"publish"}]'
    frame = msgpack.packb([1, 2, 3, 4.5])
    await ws.send_bytes(frame)
    assert await ws.receive_bytes() == frame
    await ws.close()
    assert "/nt/websim" in nt.paths
    assert nt.protocols[-1] == "v4.1.networktables.first.wpi.edu"


async def test_nt_proxy_without_robot(cfg, aiohttp_client):
    client = await aiohttp_client(create_app(cfg))
    assert (await client.get("/nt/websim", headers={"Upgrade": "websocket"})).status == 502


def test_stream_port_parsing():
    assert stream_port(["mjpg:http://127.0.0.1:1181/?action=stream"]) == 1181
    assert stream_port(["mjpg:http://10.0.0.2:1182/stream.mjpg", "x"]) == 1182
    assert stream_port(["usb:/dev/video0"]) is None
    assert stream_port([]) is None
    assert stream_port([5, "mjpg:http://[::1]:1183/"]) == 1183


async def test_cameras_are_discovered_from_networktables(cfg, aiohttp_server, aiohttp_client):
    nt = FakeNt(streams=["mjpg:http://127.0.0.1:5999/?action=stream"])
    cfg.nt_port = (await aiohttp_server(nt.app())).port
    client = await aiohttp_client(create_app(cfg))
    status = client.app[STATUS]
    await wait_for(lambda: status.cameras, timeout=5)
    assert status.running and status.wpilib_version == "2026.2.1"
    body = await (await client.get("/api/status")).json()
    assert body["nt"]["up"] is True
    assert body["cameras"] == [{"name": "frontcam-raw", "port": 5999}]


async def test_camera_proxy_streams_and_restricts_ports(cfg, aiohttp_server, aiohttp_client):
    boundary = "boundarydonotcross"

    async def stream(request: web.Request) -> web.StreamResponse:
        resp = web.StreamResponse(
            headers={"Content-Type": f"multipart/x-mixed-replace;boundary={boundary}"}
        )
        await resp.prepare(request)
        for i in range(3):
            await resp.write(
                f"--{boundary}\r\nContent-Type: image/jpeg\r\n\r\nframe{i}\r\n".encode()
            )
        return resp

    cam_app = web.Application()
    cam_app.router.add_get("/stream.mjpg", stream)
    port = (await aiohttp_server(cam_app)).port
    cfg.camera_ports = range(port, port + 1)
    client = await aiohttp_client(create_app(cfg))

    resp = await client.get(f"/cam/{port}/stream.mjpg")
    assert resp.status == 200
    assert resp.headers["Content-Type"].startswith("multipart/x-mixed-replace")
    body = await resp.read()
    assert body.count(b"frame") == 3

    # not a camera port: refused without connecting anywhere
    assert (await client.get("/cam/22/stream.mjpg")).status == 403
    assert (await client.get(f"/cam/{port + 1}/stream.mjpg")).status == 403
    assert (await client.get("/cam/abc/stream.mjpg")).status == 400


async def test_detection_does_not_connect_when_the_socket_table_is_readable(monkeypatch):
    """The robot logs every NT connection; detection must not add one a second."""
    from websimd import detect, procfs

    async def no_connections(host, port, timeout=0.5):
        raise AssertionError("detection opened a connection to the robot")

    monkeypatch.setattr(detect, "port_open", no_connections)
    monkeypatch.setattr(procfs, "port_listening", lambda port: port == 5810)
    assert await detect.is_listening("127.0.0.1", 5810) is True
    assert await detect.is_listening("127.0.0.1", 3301) is False

    # without a socket table (not Linux) a connection is the only way to know
    connected = []

    async def record(host, port, timeout=0.5):
        connected.append(port)
        return True

    monkeypatch.setattr(detect, "port_open", record)
    monkeypatch.setattr(procfs, "port_listening", lambda port: None)
    assert await detect.is_listening("127.0.0.1", 5810) is True
    assert connected == [5810]
