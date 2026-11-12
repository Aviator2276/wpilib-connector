"""Static hosting for the shell and the GUI bundles.

* ``assets/*`` are content-hashed by the build: cached forever.
* Everything else revalidates with an ETag.
* A ``.br`` or ``.gz`` sibling is sent when the browser accepts it, so the
  multi-megabyte ``simgui.wasm`` is compressed once at build time, not per
  request.
* ``index.html`` is also the fallback for client-side routes. The app runs
  under Coder's path prefix as well as at a subdomain root, so every URL in
  it is relative; a ``<base>`` computed from the request depth keeps those
  relative URLs pointing at the app root on deep links.
"""

from __future__ import annotations

import mimetypes
from pathlib import Path

from aiohttp import web

mimetypes.add_type("application/wasm", ".wasm")
mimetypes.add_type("text/javascript", ".js")
mimetypes.add_type("text/javascript", ".mjs")

_ENCODINGS = (("br", ".br"), ("gzip", ".gz"))


def _safe_join(root: Path, relative: str) -> Path | None:
    root_real = root.resolve()
    candidate = (root_real / relative).resolve()
    if candidate == root_real or candidate.is_relative_to(root_real):
        return candidate
    return None


def _etag(path: Path) -> str:
    st = path.stat()
    return f'"{st.st_mtime_ns:x}-{st.st_size:x}"'


def file_response(request: web.Request, path: Path, *, immutable: bool = False) -> web.Response:
    etag = _etag(path)
    cache = "public, max-age=31536000, immutable" if immutable else "no-cache"
    headers = {"ETag": etag, "Cache-Control": cache, "Vary": "Accept-Encoding"}
    if request.headers.get("If-None-Match") == etag:
        return web.Response(status=304, headers=headers)
    content_type = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
    accepted = request.headers.get("Accept-Encoding", "")
    for encoding, suffix in _ENCODINGS:
        if encoding in accepted:
            sibling = path.with_name(path.name + suffix)
            if sibling.is_file():
                headers["Content-Encoding"] = encoding
                return web.Response(
                    body=sibling.read_bytes(), content_type=content_type, headers=headers
                )
    return web.Response(body=path.read_bytes(), content_type=content_type, headers=headers)


class StaticSite:
    def __init__(self, web_dir: Path, wasm_dir: Path) -> None:
        self.web_dir = web_dir
        self.wasm_dir = wasm_dir

    async def wasm(self, request: web.Request) -> web.Response:
        path = _safe_join(self.wasm_dir, request.match_info["path"])
        if path is None or not path.is_file():
            raise web.HTTPNotFound()
        return file_response(request, path)

    async def site(self, request: web.Request) -> web.Response:
        rel = request.match_info.get("path", "")
        if rel:
            path = _safe_join(self.web_dir, rel)
            if path is not None and path.is_file():
                return file_response(request, path, immutable=rel.startswith("assets/"))
            if rel.startswith("assets/") or "." in rel.rsplit("/", 1)[-1]:
                raise web.HTTPNotFound()
        return self._index(rel)

    def _index(self, rel: str) -> web.Response:
        index = self.web_dir / "index.html"
        try:
            html = index.read_text()
        except OSError:
            raise web.HTTPNotFound(text="the web shell is not installed") from None
        depth = rel.count("/")
        base = "../" * depth or "./"
        html = html.replace("<head>", f'<head><base href="{base}">', 1)
        return web.Response(
            text=html, content_type="text/html", headers={"Cache-Control": "no-cache"}
        )
