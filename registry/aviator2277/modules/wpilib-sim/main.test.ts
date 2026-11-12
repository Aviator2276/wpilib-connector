import { describe, expect, it } from "bun:test";
import {
  findResourceInstance,
  runTerraformApply,
  runTerraformInit,
  testRequiredVariables,
} from "~test";

describe("wpilib-sim", () => {
  it("should init successfully", async () => {
    await runTerraformInit(import.meta.dir);
  });

  testRequiredVariables(import.meta.dir, {
    agent_id: "test-agent",
  });

  it("applies with defaults and wires the app to the daemon", async () => {
    const state = await runTerraformApply(import.meta.dir, {
      agent_id: "test-agent",
    });
    const app = findResourceInstance(state, "coder_app");
    expect(app.slug).toBe("wpilib-sim");
    expect(app.display_name).toBe("WPILib Simulator");
    expect(app.url).toBe("http://localhost:13300");
    expect(app.healthcheck[0].url).toBe("http://localhost:13300/healthz");
    expect(app.share).toBe("owner");
    expect(state.outputs.port.value).toBe(13300);
  });

  it("uses the configured port everywhere", async () => {
    const state = await runTerraformApply(import.meta.dir, {
      agent_id: "test-agent",
      port: "23456",
    });
    const app = findResourceInstance(state, "coder_app");
    const script = findResourceInstance(state, "coder_script");
    expect(app.url).toBe("http://localhost:23456");
    expect(app.healthcheck[0].url).toBe("http://localhost:23456/healthz");
    expect(script.script).toContain('PORT="23456"');
  });

  it("exports the opt-out switch for the Gradle init script", async () => {
    const state = await runTerraformApply(import.meta.dir, {
      agent_id: "test-agent",
    });
    const env = findResourceInstance(state, "coder_env");
    expect(env.name).toBe("WPILIB_WEBSIM");
    expect(env.value).toBe("1");
  });

  it("needs no display, software rendering or password", async () => {
    const state = await runTerraformApply(import.meta.dir, {
      agent_id: "test-agent",
    });
    const names = state.resources
      .filter((r) => r.type === "coder_env")
      .flatMap((r) => r.instances.map((i) => i.attributes.name));
    expect(names).toEqual(["WPILIB_WEBSIM"]);
    expect(state.resources.some((r) => r.type === "random_password")).toBe(
      false,
    );
    const script = findResourceInstance(state, "coder_script");
    for (const leftover of ["kasmvnc", "Xvnc", "DISPLAY", "xdotool", "feh "]) {
      expect(script.script).not.toContain(leftover);
    }
  });

  it("passes the safety and injection switches to the script", async () => {
    const state = await runTerraformApply(import.meta.dir, {
      agent_id: "test-agent",
      auto_disable: "false",
      inject_gradle: "false",
    });
    const script = findResourceInstance(state, "coder_script");
    expect(script.script).toContain('AUTO_DISABLE="false"');
    expect(script.script).toContain('INJECT_GRADLE="false"');
  });

  it("leaves shell expansions in the script intact", async () => {
    const state = await runTerraformApply(import.meta.dir, {
      agent_id: "test-agent",
    });
    const script = findResourceInstance(state, "coder_script");
    // templatefile must turn the double-dollar escapes back into shell syntax
    expect(script.script).toContain('ver="${vsix##*vscode-wpilib-}"');
    expect(script.script).not.toContain("$${");
  });
});
