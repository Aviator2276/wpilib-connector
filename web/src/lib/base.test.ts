import { describe, expect, it } from "vitest";
import { api, routerBasepath, ws } from "./base";

describe("URL helpers", () => {
  it("resolve against a subdomain root", () => {
    const base = "https://wpilib-sim--main--ws--user.coder.example.com/";
    expect(api("api/status", base)).toBe(
      "https://wpilib-sim--main--ws--user.coder.example.com/api/status",
    );
    expect(ws("nt", base)).toBe("wss://wpilib-sim--main--ws--user.coder.example.com/nt");
    expect(routerBasepath(base)).toBe("/");
  });

  it("stay under Coder's path prefix", () => {
    const base = "https://coder.example.com/@user/ws.main/apps/wpilib-sim/";
    expect(api("api/status", base)).toBe(
      "https://coder.example.com/@user/ws.main/apps/wpilib-sim/api/status",
    );
    expect(api("/wasm/2026.2.1/simgui.js", base)).toBe(
      "https://coder.example.com/@user/ws.main/apps/wpilib-sim/wasm/2026.2.1/simgui.js",
    );
    expect(ws("hal", base)).toBe("wss://coder.example.com/@user/ws.main/apps/wpilib-sim/hal");
    expect(routerBasepath(base)).toBe("/@user/ws.main/apps/wpilib-sim");
  });

  it("use ws: for plain http", () => {
    expect(ws("hal", "http://localhost:13300/")).toBe("ws://localhost:13300/hal");
  });
});
