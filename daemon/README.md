# websimd

The workspace-side daemon of the WPILib Web Sim. It serves the browser app and
relays NetworkTables, HAL state and camera streams between the browser and the
robot program running in the same workspace.

```bash
uv sync
uv run websimd --web-dir ../web/dist --wasm-dir ../wasm/dist
```

```bash
uv run pytest
```

Flags are listed by `websimd --help`; each has a `WEBSIMD_*` environment
fallback. The HTTP and WebSocket API is described in
[`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).
