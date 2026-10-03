import { describe, expect, it } from "vitest";
import { demoSnapshot } from "../data/demo";
import { sha256Hex } from "../crypto/portable";
import { buildReport, canonicalize } from "./report";

describe("deterministic evidence report", () => {
  it("keeps every event fact traceable", async () => {
    const report = await buildReport(demoSnapshot, "M12");
    expect(report.markdown).toContain("[E:experiment/M12/1]");
    expect(report.markdown).toContain("Evidence SHA-256");
    expect(report.digest).toMatch(/^[a-f0-9]{64}$/);
    expect(report.evidence.integrity.complete).toBe(true);
  });

  it("digest matches exported canonical evidence bytes", async () => {
    const report = await buildReport(demoSnapshot, "M12");
    expect(report.digest).toBe(sha256Hex(canonicalize(report.evidence)));
  });

  it("rejects an unknown experiment", async () => {
    await expect(buildReport(demoSnapshot, "UNKNOWN")).rejects.toThrow(
      "实验 UNKNOWN 不存在",
    );
  });

  it("never marks an experiment with no facts as complete", async () => {
    const report = await buildReport(
      { ...demoSnapshot, events: [] },
      "M12",
    );
    expect(report.evidence.integrity.complete).toBe(false);
  });
});
