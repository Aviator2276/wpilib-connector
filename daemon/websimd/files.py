"""Project files the browser GUI needs: saved layouts, the file picker, uploads.

Everything here is confined to the robot project directory of the running
simulation. The user already has a shell in the workspace, so this is not an
isolation boundary; it limits what a web page that reaches the app can touch.
"""

from __future__ import annotations

import hashlib
import os
import tempfile
from pathlib import Path
from urllib.parse import unquote

from aiohttp import web

from .state import Status

# The Sim GUI's layout files (glass storage name "simgui").
STORAGE_NAMES = frozenset({"simgui.json", "simgui-ds.json", "simgui-window.json"})
MAX_STORAGE_BYTES = 4 * 1024 * 1024
MAX_FILE_BYTES = 32 * 1024 * 1024
MAX_UPLOAD_BYTES = 16 * 1024 * 1024
MAX_LISTING = 2000


def resolve_inside(root: Path, relative: str) -> Path:
    """Resolve `relative` under `root`; reject anything that escapes it.

    Symlinks are resolved first, so a link pointing out of the project is
    rejected as well.
    """
    if "\x00" in relative:
        raise web.HTTPBadRequest(text="invalid path")
    root_real = root.resolve()
    candidate = (root_real / relative.lstrip("/")).resolve()
    if candidate != root_real and not candidate.is_relative_to(root_real):
        raise web.HTTPForbidden(text="path is outside the project")
    return candidate


def atomic_write(path: Path, data: bytes) -> None:
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.", suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        # keep the mode of an existing file (mkstemp creates 0600)
        try:
            os.chmod(tmp, path.stat().st_mode & 0o777)
        except FileNotFoundError:
            os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except FileNotFoundError:
            pass
        raise


def etag_of(data: bytes) -> str:
    return '"' + hashlib.sha1(data, usedforsecurity=False).hexdigest() + '"'


class ProjectFiles:
    def __init__(self, status: Status) -> None:
        self.status = status

    def _root(self) -> Path:
        root = self.status.project
        if root is None or not root.is_dir():
            raise web.HTTPConflict(text="no robot project is known (is a simulation running?)")
        return root

    def _storage_path(self, request: web.Request) -> Path:
        name = request.match_info["name"]
        if name not in STORAGE_NAMES:
            raise web.HTTPNotFound(text="unknown storage file")
        folder = resolve_inside(self._root(), request.query.get("dir", ""))
        return folder / name

    # ------------------------------------------------------- /api/storage

    async def get_storage(self, request: web.Request) -> web.Response:
        path = self._storage_path(request)
        try:
            data = path.read_bytes()
        except (FileNotFoundError, NotADirectoryError):
            raise web.HTTPNotFound(text="not saved yet") from None
        etag = etag_of(data)
        if request.headers.get("If-None-Match") == etag:
            return web.Response(status=304, headers={"ETag": etag})
        return web.Response(
            body=data,
            content_type="application/json",
            headers={"ETag": etag, "Cache-Control": "no-store"},
        )

    async def put_storage(self, request: web.Request) -> web.Response:
        path = self._storage_path(request)
        if request.content_length is not None and request.content_length > MAX_STORAGE_BYTES:
            raise web.HTTPRequestEntityTooLarge(MAX_STORAGE_BYTES, request.content_length)
        data = await request.content.read(MAX_STORAGE_BYTES + 1)
        if len(data) > MAX_STORAGE_BYTES:
            raise web.HTTPRequestEntityTooLarge(MAX_STORAGE_BYTES, len(data))
        if_match = request.headers.get("If-Match")
        if if_match:
            try:
                current = etag_of(path.read_bytes())
            except FileNotFoundError:
                current = None
            if current != if_match:
                raise web.HTTPPreconditionFailed(text="file changed on disk")
        path.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(path, data)
        return web.Response(status=204, headers={"ETag": etag_of(data)})

    # ------------------------------------------------------------ /api/fs

    async def list_dir(self, request: web.Request) -> web.Response:
        root = self._root()
        folder = resolve_inside(root, request.query.get("dir", ""))
        if not folder.is_dir():
            raise web.HTTPNotFound(text="no such directory")
        entries = []
        try:
            with os.scandir(folder) as it:
                for entry in it:
                    if len(entries) >= MAX_LISTING:
                        break
                    try:
                        is_dir = entry.is_dir()
                        size = 0 if is_dir else entry.stat().st_size
                    except OSError:
                        continue
                    entries.append(
                        {"name": entry.name, "type": "dir" if is_dir else "file", "size": size}
                    )
        except PermissionError:
            raise web.HTTPForbidden(text="cannot read directory") from None
        entries.sort(key=lambda e: (e["type"] != "dir", e["name"].lower()))
        root_real = root.resolve()
        rel = folder.relative_to(root_real).as_posix()
        return web.json_response(
            {
                "root": str(root),
                "dir": "" if rel == "." else rel,
                "entries": entries,
            },
            headers={"Cache-Control": "no-store"},
        )

    async def get_file(self, request: web.Request) -> web.StreamResponse:
        path = resolve_inside(self._root(), request.query.get("path", ""))
        if not path.is_file():
            raise web.HTTPNotFound(text="no such file")
        if path.stat().st_size > MAX_FILE_BYTES:
            raise web.HTTPRequestEntityTooLarge(MAX_FILE_BYTES, path.stat().st_size)
        return web.FileResponse(path, headers={"Cache-Control": "no-store"})

    async def upload(self, request: web.Request) -> web.Response:
        """Multipart upload of one or more files into ?dir= (default: project root)."""
        root = self._root()
        folder = resolve_inside(root, request.query.get("dir", ""))
        if not folder.is_dir():
            raise web.HTTPNotFound(text="no such directory")
        reader = await request.multipart()
        saved = []
        total = 0
        async for part in reader:
            if part.filename is None:
                continue
            # Browsers send a bare name; anything path-like (also when
            # percent-encoded) is reduced to its last component.
            name = os.path.basename(unquote(part.filename).replace("\\", "/"))
            if not name or name in (".", ".."):
                raise web.HTTPBadRequest(text="invalid file name")
            target = resolve_inside(root, str((folder / name).relative_to(root.resolve())))
            chunks = []
            while chunk := await part.read_chunk(64 * 1024):
                total += len(chunk)
                if total > MAX_UPLOAD_BYTES:
                    raise web.HTTPRequestEntityTooLarge(MAX_UPLOAD_BYTES, total)
                chunks.append(chunk)
            atomic_write(target, b"".join(chunks))
            saved.append(target.relative_to(root.resolve()).as_posix())
        if not saved:
            raise web.HTTPBadRequest(text="no file in upload")
        return web.json_response({"saved": saved})
