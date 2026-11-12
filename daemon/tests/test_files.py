"""Layout storage, the file picker listing, uploads, and path confinement."""

from __future__ import annotations

import os

import aiohttp
import pytest
from aiohttp import web

from websimd.app import STATUS, create_app
from websimd.files import resolve_inside


async def test_storage_round_trip_with_etag(cfg, project, aiohttp_client):
    client = await aiohttp_client(create_app(cfg))
    assert (await client.get("/api/storage/simgui.json")).status == 404

    put = await client.put("/api/storage/simgui.json", data=b'{"a": 1}')
    assert put.status == 204
    assert (project / "simgui.json").read_bytes() == b'{"a": 1}'

    got = await client.get("/api/storage/simgui.json")
    assert await got.read() == b'{"a": 1}'
    etag = got.headers["ETag"]
    assert etag == put.headers["ETag"]
    assert (
        await client.get("/api/storage/simgui.json", headers={"If-None-Match": etag})
    ).status == 304

    # optimistic concurrency: a stale If-Match is refused
    (project / "simgui.json").write_text('{"edited": "in vscode"}')
    stale = await client.put("/api/storage/simgui.json", data=b"{}", headers={"If-Match": etag})
    assert stale.status == 412
    assert (project / "simgui.json").read_text() == '{"edited": "in vscode"}'


async def test_storage_only_accepts_the_three_layout_files(cfg, project, aiohttp_client):
    client = await aiohttp_client(create_app(cfg))
    for name in ("simgui.json", "simgui-ds.json", "simgui-window.json"):
        assert (await client.put(f"/api/storage/{name}", data=b"{}")).status == 204
    for name in ("build.gradle", "networktables.json", ".bashrc", "simgui.json.bak"):
        assert (await client.put(f"/api/storage/{name}", data=b"x")).status == 404
        assert (await client.get(f"/api/storage/{name}")).status == 404
    assert (project / "build.gradle").read_text().startswith("plugins")


async def test_storage_in_a_subfolder_for_workspace_save(cfg, project, aiohttp_client):
    client = await aiohttp_client(create_app(cfg))
    resp = await client.put("/api/storage/simgui.json?dir=layouts/match", data=b"{}")
    assert resp.status == 204
    assert (project / "layouts" / "match" / "simgui.json").exists()
    escape = await client.put("/api/storage/simgui.json?dir=../elsewhere", data=b"{}")
    assert escape.status == 403
    assert not (project.parent / "elsewhere").exists()


async def test_storage_size_cap(cfg, aiohttp_client):
    client = await aiohttp_client(create_app(cfg))
    resp = await client.put("/api/storage/simgui.json", data=b"x" * (4 * 1024 * 1024 + 1))
    assert resp.status == 413


async def test_no_project_known(cfg, aiohttp_client):
    client = await aiohttp_client(create_app(cfg))
    client.app[STATUS].project = None
    assert (await client.get("/api/storage/simgui.json")).status == 409
    assert (await client.get("/api/fs")).status == 409


async def test_listing(cfg, project, aiohttp_client):
    client = await aiohttp_client(create_app(cfg))
    body = await (await client.get("/api/fs")).json()
    assert body["dir"] == ""
    names = [(e["name"], e["type"]) for e in body["entries"]]
    assert names[:2] == [(".wpilib", "dir"), ("src", "dir")]  # directories first
    assert ("build.gradle", "file") in names
    sub = await (await client.get("/api/fs?dir=src")).json()
    assert sub["dir"] == "src"
    assert sub["entries"] == [{"name": "field.png", "type": "file", "size": 9}]
    assert (await client.get("/api/fs?dir=nope")).status == 404


@pytest.mark.parametrize("path", ["..", "../", "../../etc", "src/../../x", "/../outside", "a\x00b"])
async def test_paths_cannot_leave_the_project(cfg, aiohttp_client, path):
    client = await aiohttp_client(create_app(cfg))
    assert (await client.get("/api/fs", params={"dir": path})).status in (400, 403, 404)
    assert (await client.get("/api/fs/file", params={"path": path})).status in (400, 403, 404)


def test_absolute_paths_are_taken_relative_to_the_project(project):
    assert resolve_inside(project, "/src") == project.resolve() / "src"


def test_symlink_out_of_project_is_rejected(project, tmp_path):
    secret = tmp_path / "secret.txt"
    secret.write_text("no")
    os.symlink(secret, project / "link.txt")
    os.symlink(tmp_path, project / "linkdir")
    with pytest.raises(web.HTTPForbidden):
        resolve_inside(project, "link.txt")
    with pytest.raises(web.HTTPForbidden):
        resolve_inside(project, "linkdir/secret.txt")


async def test_download_and_upload(cfg, project, aiohttp_client):
    client = await aiohttp_client(create_app(cfg))
    got = await client.get("/api/fs/file?path=src/field.png")
    assert await got.read() == b"\x89PNG fake"

    form = aiohttp.FormData()
    form.add_field("file", b"JPEGDATA", filename="../../evil/field.jpg")
    resp = await client.post("/api/fs/upload?dir=src", data=form)
    assert resp.status == 200
    # only the base name of the upload is used
    assert (await resp.json())["saved"] == ["src/field.jpg"]
    assert (project / "src" / "field.jpg").read_bytes() == b"JPEGDATA"
    assert not (project.parent / "evil").exists()

    form = aiohttp.FormData()
    form.add_field("file", b"x", filename="a.png")
    assert (await client.post("/api/fs/upload?dir=../..", data=form)).status == 403
