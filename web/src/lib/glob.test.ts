import { describe, expect, it } from "vitest";
import { fileMatcher } from "./glob";

describe("fileMatcher", () => {
  it("matches the filter list Glass uses for field images", () => {
    const m = fileMatcher(["*.jpg *.jpeg *.png *.json"]);
    expect(m("2026-field.png")).toBe(true);
    expect(m("FIELD.JPG")).toBe(true);
    expect(m("layout.json")).toBe(true);
    expect(m("build.gradle")).toBe(false);
    expect(m("png")).toBe(false);
  });

  it("treats no filter and * as everything", () => {
    expect(fileMatcher([])("anything")).toBe(true);
    expect(fileMatcher(["*"])("anything.bin")).toBe(true);
  });

  it("does not treat dots or brackets as regex syntax", () => {
    const m = fileMatcher(["a.b[1].txt"]);
    expect(m("a.b[1].txt")).toBe(true);
    expect(m("aXb1.txt")).toBe(false);
  });
});
