"""HAL relay: cache, coalescing, snapshot-on-connect, echo, auto-disable."""

from __future__ import annotations

import asyncio
import json

from fake_robot import FakeHalWs, FakeNt

from websimd.app import STATUS, create_app
from websimd.halrelay import HalCache

INITIAL = [
    {"type": "PWM", "device": "0", "data": {"<init": True, "<speed": 0.0}},
    {"type": "DriverStation", "device": "", "data": {">enabled": False, ">ds": False}},
    {
        "type": "CANMotor",
        "device": "Talon FX (v6)[1]",
        "data": {">supplyVoltage": 12.0, "<dutyCycle": 0.0},
    },
]


def by_device(batch: list[dict]) -> dict:
    return {(m["type"], m["device"]): m["data"] for m in batch}


async def wait_for(predicate, timeout: float = 3.0) -> None:
    async with asyncio.timeout(timeout):
        while not predicate():  # noqa: ASYNC110 - polling plain attributes
            await asyncio.sleep(0.01)


async def start(cfg, aiohttp_server, aiohttp_client, robot: FakeHalWs):
    server = await aiohttp_server(robot.app())
    cfg.hal_ws_port = server.port
    # the detector treats an open NetworkTables port as "robot program running"
    cfg.nt_port = (await aiohttp_server(FakeNt().app())).port
    client = await aiohttp_client(create_app(cfg))
    status = client.app[STATUS]
    await wait_for(lambda: status.hal_up, timeout=5)
    return client, status


def test_cache_marks_only_changes_dirty():
    cache = HalCache()
    cache.update({"type": "PWM", "device": "0", "data": {"<speed": 0.5, "<init": True}})
    assert by_device(cache.take_dirty()) == {("PWM", "0"): {"<speed": 0.5, "<init": True}}
    # same value again: nothing to send
    cache.update({"type": "PWM", "device": "0", "data": {"<speed": 0.5}})
    assert cache.take_dirty() == []
    # forty changes in one window collapse to the last value
    for i in range(40):
        cache.update({"type": "PWM", "device": "0", "data": {"<speed": i / 40}})
    assert by_device(cache.take_dirty()) == {("PWM", "0"): {"<speed": 39 / 40}}
    assert by_device(cache.snapshot())[("PWM", "0")] == {"<speed": 39 / 40, "<init": True}


def test_cache_never_stores_one_shot_keys():
    cache = HalCache()
    cache.update(
        {"type": "DriverStation", "device": "", "data": {">new_data": True, ">enabled": True}}
    )
    assert by_device(cache.snapshot()) == {("DriverStation", ""): {">enabled": True}}
    assert by_device(cache.take_dirty()) == {("DriverStation", ""): {">enabled": True}}


async def test_snapshot_on_connect_and_coalesced_updates(cfg, aiohttp_server, aiohttp_client):
    robot = FakeHalWs(INITIAL)
    client, status = await start(cfg, aiohttp_server, aiohttp_client, robot)
    assert status.hal_mode == "ws"
    await asyncio.sleep(0.1)  # let the initial state land in the cache

    ws = await client.ws_connect("/hal")
    snapshot = by_device(json.loads(await ws.receive_str()))
    assert snapshot[("PWM", "0")] == {"<init": True, "<speed": 0.0}
    assert snapshot[("CANMotor", "Talon FX (v6)[1]")][">supplyVoltage"] == 12.0

    # a burst from the robot arrives as one batch holding only the last value
    for i in range(50):
        await robot.send("CANMotor", "Talon FX (v6)[1]", {">supplyVoltage": 12.0 + i / 100})
    batch = by_device(json.loads(await ws.receive_str()))
    assert set(batch) == {("CANMotor", "Talon FX (v6)[1]")}
    updates = [batch[("CANMotor", "Talon FX (v6)[1]")][">supplyVoltage"]]
    while updates[-1] != 12.49:
        more = by_device(json.loads(await ws.receive_str(timeout=1)))
        updates.append(more[("CANMotor", "Talon FX (v6)[1]")][">supplyVoltage"])
    assert len(updates) < 10, "50 changes must not produce 50 frames"
    await ws.close()


async def test_only_one_upstream_connection_for_many_tabs(cfg, aiohttp_server, aiohttp_client):
    robot = FakeHalWs(INITIAL)
    client, status = await start(cfg, aiohttp_server, aiohttp_client, robot)
    tabs = [await client.ws_connect("/hal") for _ in range(3)]
    for t in tabs:
        await t.receive_str()
    await wait_for(lambda: status.clients == 3)
    assert robot.connections == 1
    for t in tabs:
        await t.close()


async def test_browser_input_is_forwarded_and_echoed_to_other_tabs(
    cfg, aiohttp_server, aiohttp_client
):
    robot = FakeHalWs(INITIAL)
    client, _ = await start(cfg, aiohttp_server, aiohttp_client, robot)
    a = await client.ws_connect("/hal")
    b = await client.ws_connect("/hal")
    await a.receive_str()
    await b.receive_str()

    await a.send_str(
        json.dumps(
            [
                {"type": "DriverStation", "device": "", "data": {">enabled": True}},
                {"type": "Joystick", "device": "0", "data": {">axes": [0.5]}},
                {"type": "DriverStation", "device": "", "data": {">new_data": True}},
                "garbage",
                {"type": 5, "device": "", "data": {}},
            ]
        )
    )
    await wait_for(lambda: len(robot.received) == 3)
    # one object per frame, in order, invalid entries dropped
    assert [m["type"] for m in robot.received] == ["DriverStation", "Joystick", "DriverStation"]
    assert robot.received[2]["data"] == {">new_data": True}

    echoed = json.loads(await b.receive_str(timeout=1))
    # the other tab sees the state change, but not the one-shot or the sticks
    assert echoed == [{"type": "DriverStation", "device": "", "data": {">enabled": True}}]

    # and a tab that connects later starts from it
    c = await client.ws_connect("/hal")
    assert by_device(json.loads(await c.receive_str()))[("DriverStation", "")][">enabled"] is True
    for t in (a, b, c):
        await t.close()


async def test_robot_is_disabled_after_last_tab_closes(cfg, aiohttp_server, aiohttp_client):
    robot = FakeHalWs(INITIAL)
    client, status = await start(cfg, aiohttp_server, aiohttp_client, robot)
    a = await client.ws_connect("/hal")
    b = await client.ws_connect("/hal")
    await a.close()
    await asyncio.sleep(cfg.disable_grace + 0.2)
    assert robot.received == [], "another tab is still open"
    await b.close()
    await wait_for(lambda: len(robot.received) == 2)
    assert robot.received[0]["data"] == {">enabled": False}
    assert robot.received[1]["data"] == {">new_data": True}
    assert status.clients == 0


async def test_returning_tab_cancels_the_disable(cfg, aiohttp_server, aiohttp_client):
    robot = FakeHalWs(INITIAL)
    cfg.disable_grace = 0.4
    client, _ = await start(cfg, aiohttp_server, aiohttp_client, robot)
    a = await client.ws_connect("/hal")
    await a.close()
    await asyncio.sleep(0.1)
    b = await client.ws_connect("/hal")  # e.g. a page reload
    await asyncio.sleep(0.6)
    assert robot.received == []
    await b.close()


async def test_auto_disable_can_be_turned_off(cfg, aiohttp_server, aiohttp_client):
    robot = FakeHalWs(INITIAL)
    cfg.auto_disable = False
    client, _ = await start(cfg, aiohttp_server, aiohttp_client, robot)
    a = await client.ws_connect("/hal")
    await a.close()
    await asyncio.sleep(cfg.disable_grace + 0.3)
    assert robot.received == []
    # the explicit endpoint still works
    resp = await client.post("/api/sim/disable")
    assert resp.status == 200
    await wait_for(lambda: len(robot.received) == 2)


async def test_hal_refused_while_no_robot(cfg, aiohttp_client):
    client = await aiohttp_client(create_app(cfg))
    resp = await client.get("/hal")
    assert resp.status == 503
    assert (await client.post("/api/sim/disable")).status == 409
