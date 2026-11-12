from __future__ import annotations

from pathlib import Path

import pytest

from websimd.config import Config


@pytest.fixture
def project(tmp_path: Path) -> Path:
    """A minimal robot project directory."""
    root = tmp_path / "robot"
    (root / ".wpilib").mkdir(parents=True)
    (root / ".wpilib" / "wpilib_preferences.json").write_text('{"projectYear": "2026"}')
    (root / "build.gradle").write_text(
        'plugins {\n    id "java"\n    id "edu.wpi.first.GradleRIO" version "2026.2.1"\n}\n'
    )
    (root / "src").mkdir()
    (root / "src" / "field.png").write_bytes(b"\x89PNG fake")
    return root


@pytest.fixture
def site(tmp_path: Path) -> tuple[Path, Path]:
    web = tmp_path / "web"
    (web / "assets").mkdir(parents=True)
    (web / "index.html").write_text("<!doctype html><html><head><title>t</title></head></html>")
    (web / "assets" / "app-abc123.js").write_text("console.log(1)")
    wasm = tmp_path / "wasm"
    for version in ("2026.1.1", "2026.2.1"):
        d = wasm / version / "fields"
        d.mkdir(parents=True)
        (wasm / version / "simgui.wasm").write_bytes(b"\0asm" + version.encode())
        (wasm / version / "simgui.wasm.br").write_bytes(b"brotli" + version.encode())
        (wasm / version / "simgui.js").write_text("export default 1")
        (d / "index.json").write_text(
            '[{"name": "2026 Rebuilt", "json": "2026-rebuilt.json", "image": "2026-field.png"}]'
        )
        (d / "2026-rebuilt.json").write_text("{}")
    return web, wasm


@pytest.fixture
def cfg(project: Path, site: tuple[Path, Path], unused_tcp_port_factory) -> Config:
    web, wasm = site
    # Robot ports point at nothing by default; tests that need a fake robot
    # overwrite them.
    return Config(
        web_dir=web,
        wasm_dir=wasm,
        project=project,
        nt_port=unused_tcp_port_factory(),
        hal_ws_port=unused_tcp_port_factory(),
        bridge_port=unused_tcp_port_factory(),
        disable_grace=0.2,
    )
