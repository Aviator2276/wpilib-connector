# Web sim protocols

Three things travel between the browser and the robot program, all through
the daemon's single HTTP origin.

| Path | Carries | Between |
|---|---|---|
| `/nt/<client-id>` | NetworkTables 4.1, untouched | ntcore in the browser ↔ the robot's NT server (`:5810`) |
| `/hal` | HAL state, one of the two encodings below | mirror HAL in the browser ↔ the robot's HAL |
| `/cam/<port>/stream.mjpg` | MJPEG | `<img>` in the shell ← cscore in the robot process |

The mirror tells the two HAL encodings apart by frame type: text frames are
JSON (stock), binary frames are the bridge protocol.

## The HAL field table

`hal_fields.def` lists every mirrored HAL value once: device class, the
`HALSIM_` accessor names, value type, direction, and its key in the stock JSON
protocol if it has one. `hal_table.h` expands it into typed setters and
callback registrations. Both the browser mirror (`wasm/src/mirror*.cpp`) and
the robot-side bridge (`bridge/src/bridge.cpp`) include it, so:

- a row that does not match the HAL headers fails to compile on both sides;
- classes and fields are numbered by position, and the two ends compare a
  hash of the table on connect (`HalTableHash()`), so a mirror never decodes
  a bridge built from a different table.

Direction decides who may write a value: `OUT` is written by the robot program
and read-only in the GUI; `IN` is an input to the robot program that the GUI
edits; `BIDIR` is both.

## Stock protocol (`halsim_ws`, JSON)

WPILib's own `halsim_ws_server`, specified in allwpilib
`simulation/halsim_ws_core/doc/hardware_ws_api.md`: one JSON object per text
frame, `{"type", "device", "data"}`, keys prefixed `<` (robot output), `>`
(robot input) or `<>`.

The server accepts exactly one client, so the daemon owns that connection and
fans out. Between the daemon and a browser tab:

- frames are JSON **arrays** of those message objects (a single object is
  also accepted);
- on connect the tab receives the cached state of every device as a snapshot;
- robot-to-browser changes are merged per key and flushed every 33 ms;
- browser-to-robot messages are forwarded immediately, one object per frame,
  and echoed to the other tabs (except joystick input, which is per tab);
- `>new_data` and the `HAL` type's periodic markers are events, never cached.

The mirror is the inverse of WPILib's providers: it applies every key and
sends only GUI edits of `>` and `<>` values. A SimDevice whose name has a
prefix (`CANMotor:Talon FX (v6)[1]`) arrives split into `type` and `device`;
the mirror recreates it under its full name, which is what the GUI sorts by.

What this protocol cannot carry: timing (pause, step, FPGA time, notifiers),
Power Distribution, REV PH, analog gyros, duty cycle inputs, SimValue types
(every number is a double) and enum option names.

## Bridge protocol (`halsim_webbridge`, binary)

Defined in `bridge_protocol.h`, which also contains the encoder and decoder
both ends use.

- WebSocket server in the robot process on `127.0.0.1:3301`
  (`WEBSIM_BRIDGE_PORT`). `/` is the data path and takes any number of
  clients; `/control` takes JSON text commands and is sent no data.
- A binary frame is a MessagePack array of messages; a message is an array
  whose first element is the opcode.
- A new client first receives the complete state, ending with `SnapshotEnd`.
- Changes are sent last-value-wins every 20 ms. A change made by one client
  goes to the others, not back to its author.
- The robot's clock and notifier list are sent ten times a second.

| Op | Direction | Elements after the opcode |
|---|---|---|
| 0 `Hello` | robot → GUI | protocol version, HAL table hash, WPILib version |
| 1 `Field` | both | field index, device index, value |
| 2 `SimDevNew` | robot → GUI | device id, name |
| 3 `SimDevFree` | robot → GUI | device id |
| 4 `SimValNew` | robot → GUI | device id, value id, name, direction, HAL type, value, [enum options], [enum values] |
| 5 `SimVal` | both | value id, value |
| 6 `EncoderCh` | robot → GUI | encoder index, channel A, channel B |
| 7 `LedData` | robot → GUI | strip index, bin of r,g,b bytes |
| 8 `JoyOutputs` | robot → GUI | stick, outputs, left rumble, right rumble |
| 9 `Timing` | robot → GUI | FPGA time (µs), paused, next notifier timeout, [[name, timeout, valid]…] |
| 10 `GameData` | both | game-specific message |
| 12 `SnapshotEnd` | robot → GUI | — |
| 20 `Joystick` | GUI → robot | stick, [axes], [povs], button bits, button count |
| 21 `NewData` | GUI → robot | — (latch driver station and joystick data) |
| 22 `Pause` | GUI → robot | bool |
| 23 `Step` | GUI → robot | microseconds |

A device index is the instance number, or `instance * 256 + channel` for
per-channel fields (solenoids, power distribution currents). The bridge
refuses writes to `OUT` fields and to output SimValues. Unknown opcodes are
skipped, so either end can be extended.

Control command, sent by the daemon when the last tab has gone:

```json
{"cmd": "disable"}
```
