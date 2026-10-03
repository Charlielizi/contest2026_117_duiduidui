import { describe, expect, it } from "vitest";
import { findCursorGaps, mergeEvents, type StreamEvent } from "./schema";

const event = (seq: number): StreamEvent => ({
  stream: "experiment",
  subject_id: "M12",
  seq,
  event_id: `E-${seq}`,
  timestamp_epoch: 1000 + seq,
  uptime_ms: seq * 100,
  event_type: "TEST",
  source: "host",
  payload: {},
});

describe("event cursor handling", () => {
  it("deduplicates retransmitted events", () => {
    expect(mergeEvents([event(1)], [event(1), event(2)])).toHaveLength(2);
  });

  it("reports sequence gaps per stream", () => {
    expect(findCursorGaps([event(1), event(3)])).toEqual([
      "experiment:M12:2-2",
    ]);
  });
});
