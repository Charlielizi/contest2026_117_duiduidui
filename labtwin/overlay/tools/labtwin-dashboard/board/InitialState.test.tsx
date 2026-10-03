import { describe, expect, it } from "vitest";
import { demoSnapshot } from "../src/data/demo";
import { snapshotSchema } from "../src/protocol/schema";
import { canRestoreCachedSnapshot, initialSnapshot, navigationFromHash, previewHost } from "./initial-state";

describe("board startup state", () => {
  it("starts with no invented experiments, events, telemetry, identity or trusted clock", () => {
    const state = initialSnapshot(false);
    expect(snapshotSchema.safeParse(state).success).toBe(true);
    expect(state.experiments).toEqual([]);
    expect(state.environment_events).toEqual([]);
    expect(state.telemetry).toEqual([]);
    expect(state.events).toEqual([]);
    expect(state.device.clock_trusted).toBe(false);
    expect(state.device.sensor_stale).toBe(true);
    expect(state.device.boot_id).toBe("");
    expect(state.cursor.experiments).toEqual({});
  });
  it("only opts into samples for explicit local preview hosts", () => {
    expect(previewHost("192.168.1.41")).toBe(false);
    expect(previewHost("labtwin.local")).toBe(false);
    expect(previewHost("localhost.example.com")).toBe(false);
    expect(previewHost("localhost")).toBe(true);
    expect(previewHost("127.0.0.1")).toBe(true);
    expect(initialSnapshot(true)).toBe(demoSnapshot);
  });
  it("keeps the current page and task on refresh before writing the hash", () => {
    expect(navigationFromHash("#view=agent")).toEqual({ view: "agent", selectedId: "" });
    expect(navigationFromHash("#view=calendar&id=EXP-123")).toEqual({ view: "calendar", selectedId: "EXP-123" });
    expect(navigationFromHash("#view=unknown").view).toBe("overview");
  });
  it("does not restore a foreign or previous-boot cache, or overwrite live facts", () => {
    const identity = { device_id: demoSnapshot.device.device_id, boot_id: demoSnapshot.device.boot_id };
    expect(canRestoreCachedSnapshot(demoSnapshot, identity, false)).toBe(true);
    expect(canRestoreCachedSnapshot(demoSnapshot, identity, true)).toBe(false);
    expect(canRestoreCachedSnapshot(demoSnapshot, null, false)).toBe(false);
    expect(canRestoreCachedSnapshot(demoSnapshot, { ...identity, boot_id: "new-boot" }, false)).toBe(false);
    expect(canRestoreCachedSnapshot(demoSnapshot, { ...identity, device_id: "other-board" }, false)).toBe(false);
  });
  it("returns a fresh empty state when resetting authentication", () => {
    const first = initialSnapshot(false);
    first.experiments.push(demoSnapshot.experiments[0]);
    expect(initialSnapshot(false).experiments).toEqual([]);
  });
});
