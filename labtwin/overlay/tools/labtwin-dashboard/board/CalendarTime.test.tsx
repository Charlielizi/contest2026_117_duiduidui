import { describe, expect, it } from "vitest";
import { experimentSchema } from "../src/protocol/schema";
import { calendarDates, experimentInterval, experimentOnDay, moveCalendar } from "./calendar-time";

const epoch = (day: number) => new Date(2026, 9, day, 12).getTime() / 1000;
const task = experimentSchema.parse({ schema_version: 2, experiment_id: "EXP-test", name: "Test", state: "READY", current_step: 0, steps: [], timers: [], last_event_seq: 1, updated_epoch: epoch(10), planned_start_epoch: epoch(1), planned_end_epoch: epoch(2), started_epoch: epoch(3), ended_epoch: epoch(4) });
describe("calendar actual and planned intervals", () => {
  it("READY uses planned time and active/history tasks prefer actual time", () => {
    expect(experimentInterval(task)).toEqual({ start: epoch(1), end: epoch(2) });
    for (const state of ["RUNNING", "PAUSED", "COMPLETED", "CANCELLED"] as const) {
      expect(experimentInterval({ ...task, state })).toEqual({ start: epoch(3), end: epoch(4) });
    }
  });
  it("uses known actual end for cancellation before starting and treats zero as unknown", () => {
    expect(experimentInterval({ ...task, state: "CANCELLED", started_epoch: 0 }).start).toBe(epoch(4));
    expect(experimentInterval({ ...task, planned_start_epoch: null, planned_end_epoch: 0 }).start).toBeNull();
    expect(experimentOnDay({ ...task, planned_start_epoch: null, planned_end_epoch: null }, new Date(2026, 9, 10))).toBe(false);
  });
  it("spans local calendar days and does not show stale planned dates for history", () => {
    expect(experimentOnDay({ ...task, state: "COMPLETED" }, new Date(2026, 9, 1))).toBe(false);
    expect(experimentOnDay({ ...task, state: "COMPLETED" }, new Date(2026, 9, 4))).toBe(true);
  });
  it("constructs a seven-day week correctly across month and year boundaries", () => {
    const days = calendarDates(new Date(2026, 9, 1), "week");
    expect(days).toHaveLength(7);
    expect(days[0].getMonth()).toBe(8);
    expect(days[0].getDate()).toBe(27);
    expect(days[6].getMonth()).toBe(9);
    expect(days[6].getDate()).toBe(3);
    const newYear = calendarDates(new Date(2027, 0, 1), "week");
    expect(newYear[0].getFullYear()).toBe(2026);
    expect(newYear[6].getFullYear()).toBe(2027);
  });
  it("month navigation clamps to the first instead of skipping short months; list navigation works", () => {
    const date = moveCalendar(new Date(2026, 0, 31), "month", 1);
    expect([date.getMonth(), date.getDate()]).toEqual([1, 1]);
    expect(moveCalendar(date, "list", 1).getMonth()).toBe(2);
  });
});
