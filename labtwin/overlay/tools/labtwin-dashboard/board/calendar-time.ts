import type { Experiment } from "../src/protocol/schema";

// Zero and missing schema-v1 times are unknown, not January 1970.
const known = (value: number | null | undefined) => value && value > 0 ? value : null;

export function experimentInterval(item: Experiment) {
  const plannedStart = known(item.planned_start_epoch);
  const plannedEnd = known(item.planned_end_epoch);
  const actualStart = known(item.started_epoch);
  const actualEnd = known(item.ended_epoch);
  const start = item.state === "READY" ? plannedStart ?? plannedEnd
    : actualStart ?? actualEnd ?? plannedStart ?? plannedEnd;
  const end = item.state === "READY" ? plannedEnd ?? start : actualEnd ?? start;
  return { start, end: end && start ? Math.max(start, end) : start };
}

export function calendarDates(cursor: Date, mode: "month" | "week" | "list") {
  const start = mode === "week"
    ? new Date(cursor.getFullYear(), cursor.getMonth(), cursor.getDate() - cursor.getDay())
    : new Date(cursor.getFullYear(), cursor.getMonth(), 1);
  if (mode !== "week") start.setDate(1 - start.getDay());
  return Array.from({ length: mode === "week" ? 7 : 42 }, (_, index) => {
    const date = new Date(start);
    date.setDate(start.getDate() + index);
    return date;
  });
}

export function experimentOnDay(item: Experiment, date: Date) {
  const { start, end } = experimentInterval(item);
  if (start === null) return false;
  const next = new Date(date.getFullYear(), date.getMonth(), date.getDate() + 1);
  // Closed task interval, half-open local calendar day. DST is handled by Date.
  return start < next.getTime() / 1000 && (end ?? start) >= date.getTime() / 1000;
}

export function moveCalendar(cursor: Date, mode: "month" | "week" | "list", delta: number) {
  return mode === "week"
    ? new Date(cursor.getFullYear(), cursor.getMonth(), cursor.getDate() + delta * 7)
    : new Date(cursor.getFullYear(), cursor.getMonth() + delta, 1);
}
