import { describe, expect, it } from "vitest";
import { demoSnapshot } from "../data/demo";
import { applyEvent } from "./dashboard-reducer";
import type { StreamEvent } from "../protocol/schema";

function makeEvent(partial: Partial<StreamEvent> & Pick<StreamEvent, "stream" | "seq" | "event_type">): StreamEvent {
  return {
    subject_id: "M12",
    event_id: `evt-${partial.stream}-${partial.seq}`,
    timestamp_epoch: 1784120400,
    uptime_ms: 0,
    source: "test",
    payload: {},
    ...partial,
  };
}

describe("applyEvent", () => {
  it("把 EXPERIMENT_STARTED 投影到实验状态", () => {
    const event = makeEvent({ stream: "experiment", seq: 10, event_type: "EXPERIMENT_STARTED", payload: { state: "PAUSED" } });
    const next = applyEvent(demoSnapshot, event);
    const m12 = next.experiments.find((e) => e.experiment_id === "M12");
    expect(m12?.state).toBe("PAUSED");
    expect(m12?.last_event_seq).toBe(10);
  });

  it("把 OBSERVATION_ADDED 投影到 last_observation", () => {
    const event = makeEvent({ stream: "experiment", seq: 11, event_type: "OBSERVATION_ADDED", payload: { text: "新观察记录" } });
    const next = applyEvent(demoSnapshot, event);
    const m12 = next.experiments.find((e) => e.experiment_id === "M12");
    expect(m12?.last_observation).toBe("新观察记录");
  });

  it("把 TEMP_HIGH_CREATED 投影到环境事件（新建）", () => {
    const event = makeEvent({
      stream: "environment",
      subject_id: "global",
      seq: 5,
      event_type: "TEMP_HIGH_CREATED",
      payload: { event_id: "ENV-NEW-001", value: 31.2, threshold: 30 },
    });
    const next = applyEvent(demoSnapshot, event);
    const env = next.environment_events.find((e) => e.event_id === "ENV-NEW-001");
    expect(env).toBeDefined();
    expect(env?.state).toBe("ACTIVE");
    expect(env?.measured_value).toBe(31.2);
  });

  it("把 TEMP_HIGH_ACKNOWLEDGED 投影到环境事件状态", () => {
    const created = makeEvent({
      stream: "environment", subject_id: "global", seq: 5,
      event_type: "TEMP_HIGH_CREATED", payload: { event_id: "ENV-NEW-002", value: 30.5, threshold: 30 },
    });
    const ack = makeEvent({
      stream: "environment", subject_id: "global", seq: 6,
      event_type: "TEMP_HIGH_ACKNOWLEDGED", payload: { event_id: "ENV-NEW-002" },
    });
    let next = applyEvent(demoSnapshot, created);
    next = applyEvent(next, ack);
    const env = next.environment_events.find((e) => e.event_id === "ENV-NEW-002");
    expect(env?.state).toBe("ACKNOWLEDGED");
  });

  it("把 TEMP_HIGH_RECOVERED 投影到环境事件状态", () => {
    const created = makeEvent({
      stream: "environment", subject_id: "global", seq: 5,
      event_type: "TEMP_HIGH_CREATED", payload: { event_id: "ENV-NEW-003", value: 30.5, threshold: 30 },
    });
    const recovered = makeEvent({
      stream: "environment", subject_id: "global", seq: 7,
      event_type: "TEMP_HIGH_RECOVERED", payload: { event_id: "ENV-NEW-003" },
    });
    let next = applyEvent(demoSnapshot, created);
    next = applyEvent(next, recovered);
    const env = next.environment_events.find((e) => e.event_id === "ENV-NEW-003");
    expect(env?.state).toBe("RECOVERED");
  });

  it("幂等：重复事件不重复应用", () => {
    const event = makeEvent({ stream: "experiment", seq: 11, event_type: "OBSERVATION_ADDED", payload: { text: "重复观察" } });
    const once = applyEvent(demoSnapshot, event);
    const twice = applyEvent(once, event);
    expect(twice.events.length).toBe(once.events.length);
    expect(twice).toBe(once);
  });

  it("推进环境游标", () => {
    const event = makeEvent({
      stream: "environment", subject_id: "global", seq: 9,
      event_type: "TEMP_HIGH_CREATED", payload: { event_id: "ENV-CUR", value: 30, threshold: 30 },
    });
    const next = applyEvent(demoSnapshot, event);
    expect(next.cursor.environment).toBe(9);
  });

  it("推进实验游标", () => {
    const event = makeEvent({ stream: "experiment", seq: 20, event_type: "OBSERVATION_ADDED", payload: { text: "x" } });
    const next = applyEvent(demoSnapshot, event);
    expect(next.cursor.experiments.M12).toBe(20);
  });

  it("把 telemetry 流事件追加到遥测数组", () => {
    const event = makeEvent({
      stream: "telemetry", subject_id: "global", seq: 100,
      event_type: "TELEMETRY_AGGREGATE",
      payload: { seq: 100, timestamp_epoch: 1784120400, temperature_avg: 27.5, humidity_avg: 63, valid_samples: 30, stale_samples: 0 },
    });
    const next = applyEvent(demoSnapshot, event);
    expect(next.telemetry.some((t) => t.seq === 100)).toBe(true);
    expect(next.cursor.telemetry).toBe(100);
  });

  it("未知事件类型只合并+游标，不投影派生状态", () => {
    const event = makeEvent({ stream: "experiment", seq: 30, event_type: "SOMETHING_NEW", payload: { foo: "bar" } });
    const next = applyEvent(demoSnapshot, event);
    expect(next.events.some((e) => e.event_type === "SOMETHING_NEW")).toBe(true);
    expect(next.cursor.experiments.M12).toBe(30);
    const m12 = next.experiments.find((e) => e.experiment_id === "M12");
    const orig = demoSnapshot.experiments.find((e) => e.experiment_id === "M12");
    expect(m12?.state).toBe(orig?.state);
  });

  it("未知实验 subject_id 不崩溃", () => {
    const event = makeEvent({ stream: "experiment", subject_id: "UNKNOWN", seq: 1, event_type: "EXPERIMENT_STARTED", payload: { state: "RUNNING" } });
    const next = applyEvent(demoSnapshot, event);
    expect(next.experiments.length).toBe(demoSnapshot.experiments.length);
  });
});
