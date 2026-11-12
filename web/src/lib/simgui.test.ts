import { describe, expect, it } from "vitest";
import { referencedProjectFiles } from "./simgui";

const enc = (o: unknown) => new TextEncoder().encode(JSON.stringify(o));
const ROOT = "/home/coder/projects/robot";

describe("referencedProjectFiles", () => {
  it("finds project files a saved layout points at", () => {
    const layout = enc({
      NetworkTables: {
        types: { "/SmartDashboard/Field": "Field2d" },
        "/SmartDashboard/Field": {
          image: `${ROOT}/src/main/deploy/field.png`,
          bottom: 544,
          builtin: "2026 Rebuilt",
        },
      },
      other: [{ file: `${ROOT}/mech.json` }, { file: "/etc/passwd" }, 5, null],
    });
    expect(referencedProjectFiles([layout], ROOT).sort()).toEqual([
      "mech.json",
      "src/main/deploy/field.png",
    ]);
  });

  it("ignores paths outside the project and non-JSON input", () => {
    expect(
      referencedProjectFiles(
        [enc({ a: "/home/coder/projects/robot2/x.png" }), new Uint8Array([1, 2])],
        ROOT,
      ),
    ).toEqual([]);
  });
});
