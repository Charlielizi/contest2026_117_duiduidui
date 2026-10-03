import {
  eventKey,
  mergeEvents,
  type CursorVector,
  type DashboardSnapshot,
  type EnvironmentEvent,
  type Experiment,
  type StreamEvent,
} from "../protocol/schema";

const EXPERIMENT_STATES: Experiment["state"][] = [
  "READY",
  "RUNNING",
  "PAUSED",
  "COMPLETED",
  "CANCELLED",
  "RECOVERY_ERROR",
];

function isExperimentState(value: unknown): value is Experiment["state"] {
  return typeof value === "string" && (EXPERIMENT_STATES as string[]).includes(value);
}

/**
 * 把一条增量事件投影到完整快照。纯函数、幂等：
 * 重复事件不重复应用；未知事件类型只合并到 events 并推进游标，不投影派生状态。
 */
export function applyEvent(snapshot: DashboardSnapshot, event: StreamEvent): DashboardSnapshot {
  if (snapshot.events.some((existing) => eventKey(existing) === eventKey(event))) {
    return snapshot;
  }

  const events = mergeEvents(snapshot.events, [event]);
  const cursor = advanceCursor(snapshot.cursor, event);
  const next: DashboardSnapshot = { ...snapshot, events, cursor };

  switch (event.stream) {
    case "experiment":
      next.experiments = applyExperimentEvent(snapshot.experiments, event);
      break;
    case "environment":
      next.environment_events = applyEnvironmentEvent(snapshot.environment_events, event);
      break;
    case "telemetry":
      next.telemetry = applyTelemetryEvent(snapshot.telemetry, event);
      break;
  }

  return next;
}

function advanceCursor(cursor: CursorVector, event: StreamEvent): CursorVector {
  if (event.stream === "experiment") {
    const experiments = { ...cursor.experiments };
    experiments[event.subject_id] = Math.max(experiments[event.subject_id] ?? 0, event.seq);
    return { ...cursor, experiments };
  }
  if (event.stream === "environment") {
    return { ...cursor, environment: Math.max(cursor.environment, event.seq) };
  }
  return { ...cursor, telemetry: Math.max(cursor.telemetry, event.seq) };
}

function applyExperimentEvent(
  experiments: DashboardSnapshot["experiments"],
  event: StreamEvent,
): DashboardSnapshot["experiments"] {
  const index = experiments.findIndex((item) => item.experiment_id === event.subject_id);
  if (index === -1) return experiments;

  const current = experiments[index];
  const payload = event.payload;
  const updatedEpoch = event.timestamp_epoch ?? current.updated_epoch;
  let next: Experiment = {
    ...current,
    last_event_seq: Math.max(current.last_event_seq, event.seq),
    updated_epoch: updatedEpoch,
  };

  switch (event.event_type) {
    case "EXPERIMENT_CREATED":
    case "EXPERIMENT_STARTED":
    case "EXPERIMENT_PAUSED":
    case "EXPERIMENT_COMPLETED":
    case "EXPERIMENT_CANCELLED":
      if (isExperimentState(payload.state)) {
        next = { ...next, state: payload.state };
      }
      break;
    case "OBSERVATION_ADDED":
      if (typeof payload.text === "string") {
        next = { ...next, last_observation: payload.text };
      }
      break;
    default:
      break;
  }

  const result = [...experiments];
  result[index] = next;
  return result;
}

function applyEnvironmentEvent(
  events: DashboardSnapshot["environment_events"],
  event: StreamEvent,
): DashboardSnapshot["environment_events"] {
  const payload = event.payload;
  const targetId = typeof payload.event_id === "string" ? payload.event_id : null;
  const index = targetId ? events.findIndex((item) => item.event_id === targetId) : -1;

  switch (event.event_type) {
    case "TEMP_HIGH_CREATED":
    case "ENVIRONMENT_CREATED": {
      const value = typeof payload.value === "number" ? payload.value : 0;
      const threshold = typeof payload.threshold === "number" ? payload.threshold : 0;
      if (index >= 0) {
        const updated: EnvironmentEvent = {
          ...events[index],
          state: "ACTIVE",
          measured_value: value,
          trigger_threshold: threshold,
          created_epoch: event.timestamp_epoch ?? events[index].created_epoch,
        };
        const result = [...events];
        result[index] = updated;
        return result;
      }
      const created: EnvironmentEvent = {
        event_id: targetId ?? event.event_id,
        rule_id: typeof payload.rule_id === "string" ? payload.rule_id : "TEMP_HIGH",
        severity: typeof payload.severity === "string" ? payload.severity : "WARNING",
        state: "ACTIVE",
        sensor: typeof payload.sensor === "string" ? payload.sensor : "temperature",
        measured_value: value,
        trigger_threshold: threshold,
        clear_threshold: typeof payload.clear_threshold === "number" ? payload.clear_threshold : threshold - 2,
        created_epoch: event.timestamp_epoch,
        acknowledged_epoch: null,
        recovered_epoch: null,
        ack_source: null,
        experiment_id: event.subject_id !== "global" ? event.subject_id : null,
      };
      return [...events, created];
    }
    case "TEMP_HIGH_ACKNOWLEDGED":
    case "ENVIRONMENT_ACKNOWLEDGED": {
      if (index < 0) return events;
      const updated: EnvironmentEvent = {
        ...events[index],
        state: "ACKNOWLEDGED",
        acknowledged_epoch: event.timestamp_epoch,
        ack_source: event.source,
      };
      const result = [...events];
      result[index] = updated;
      return result;
    }
    case "TEMP_HIGH_RECOVERED":
    case "ENVIRONMENT_RECOVERED": {
      if (index < 0) return events;
      const updated: EnvironmentEvent = {
        ...events[index],
        state: "RECOVERED",
        recovered_epoch: event.timestamp_epoch,
      };
      const result = [...events];
      result[index] = updated;
      return result;
    }
    default:
      return events;
  }
}

function applyTelemetryEvent(
  telemetry: DashboardSnapshot["telemetry"],
  event: StreamEvent,
): DashboardSnapshot["telemetry"] {
  const p = event.payload;
  if (typeof p.seq !== "number" || typeof p.timestamp_epoch !== "number") {
    return telemetry;
  }
  if (telemetry.some((point) => point.seq === p.seq)) {
    return telemetry;
  }
  return [
    ...telemetry,
    {
      seq: p.seq,
      timestamp_epoch: p.timestamp_epoch,
      temperature_avg: typeof p.temperature_avg === "number" ? p.temperature_avg : null,
      humidity_avg: typeof p.humidity_avg === "number" ? p.humidity_avg : null,
      temperature_min: typeof p.temperature_min === "number" ? p.temperature_min : undefined,
      temperature_max: typeof p.temperature_max === "number" ? p.temperature_max : undefined,
      humidity_min: typeof p.humidity_min === "number" ? p.humidity_min : undefined,
      humidity_max: typeof p.humidity_max === "number" ? p.humidity_max : undefined,
      valid_samples: typeof p.valid_samples === "number" ? p.valid_samples : 0,
      stale_samples: typeof p.stale_samples === "number" ? p.stale_samples : 0,
    },
  ];
}
