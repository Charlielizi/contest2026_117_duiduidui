import { z } from "zod";

export const timerSchema = z.object({
  timer_id: z.string(),
  label: z.string(),
  step_index: z.number().int().nonnegative(),
  state: z.enum(["RUNNING", "PAUSED", "EXPIRED", "CANCELLED"]),
  remaining_seconds: z.number().int().nonnegative(),
  due_epoch: z.number().int().nullable(),
});

export const experimentSchema = z.object({
  schema_version: z.number().int(),
  experiment_id: z.string(),
  name: z.string(),
  description: z.string().default(""),
  state: z.enum([
    "READY",
    "RUNNING",
    "PAUSED",
    "COMPLETED",
    "CANCELLED",
    "RECOVERY_ERROR",
  ]),
  current_step: z.number().int().nonnegative(),
  steps: z.array(z.object({ title: z.string(), completed: z.boolean() })),
  timers: z.array(timerSchema),
  last_observation: z.string().default(""),
  last_event_seq: z.number().int().nonnegative(),
  snapshot_pending: z.boolean().default(false),
  created_epoch: z.number().int().nullable().default(null),
  planned_start_epoch: z.number().int().nullable().default(null),
  planned_end_epoch: z.number().int().nullable().default(null),
  started_epoch: z.number().int().nullable().default(null),
  ended_epoch: z.number().int().nullable().default(null),
  updated_epoch: z.number().int().nullable(),
});

export const environmentEventSchema = z.object({
  event_id: z.string(),
  rule_id: z.string(),
  severity: z.string().default("WARNING"),
  state: z.enum(["ACTIVE", "ACKNOWLEDGED", "RECOVERED"]),
  sensor: z.string(),
  measured_value: z.number(),
  trigger_threshold: z.number(),
  clear_threshold: z.number(),
  created_epoch: z.number().int().nullable(),
  acknowledged_epoch: z.number().int().nullable(),
  recovered_epoch: z.number().int().nullable(),
  ack_source: z.string().nullable(),
  experiment_id: z.string().nullable(),
});

export const telemetryPointSchema = z.object({
  seq: z.number().int().nonnegative(),
  timestamp_epoch: z.number().int(),
  temperature_avg: z.number().nullable(),
  humidity_avg: z.number().nullable(),
  temperature_min: z.number().nullable().optional(),
  temperature_max: z.number().nullable().optional(),
  humidity_min: z.number().nullable().optional(),
  humidity_max: z.number().nullable().optional(),
  valid_samples: z.number().int().nonnegative(),
  stale_samples: z.number().int().nonnegative(),
});

export const streamEventSchema = z.object({
  stream: z.enum(["experiment", "environment", "telemetry"]),
  subject_id: z.string(),
  seq: z.number().int().nonnegative(),
  event_id: z.string(),
  timestamp_epoch: z.number().int().nullable(),
  uptime_ms: z.number().nonnegative(),
  event_type: z.string(),
  source: z.string(),
  payload: z.record(z.string(), z.unknown()),
});

export const cursorVectorSchema = z.object({
  boot_id: z.string(),
  experiments: z.record(z.string(), z.number().int().nonnegative()),
  environment: z.number().int().nonnegative(),
  telemetry: z.number().int().nonnegative(),
});

export const snapshotSchema = z.object({
  device: z.object({
    device_id: z.string(),
    boot_id: z.string(),
    firmware: z.string(),
    protocol: z.literal("labtwin.dashboard.v1"),
    clock_trusted: z.boolean(),
    storage_error: z.boolean(),
    sampled_epoch: z.number().int().nullable(),
    sensor_stale: z.boolean(),
  }),
  experiments: z.array(experimentSchema),
  environment_events: z.array(environmentEventSchema),
  telemetry: z.array(telemetryPointSchema),
  events: z.array(streamEventSchema),
  cursor: cursorVectorSchema,
});

export const responseEnvelopeSchema = z.object({
  protocol: z.literal("labtwin.dashboard.v1"),
  message_id: z.string(),
  kind: z.enum(["response", "event", "error"]),
  op: z.string(),
  schema_version: z.literal(1),
  payload: z.unknown(),
});

export type Experiment = z.infer<typeof experimentSchema>;
export type EnvironmentEvent = z.infer<typeof environmentEventSchema>;
export type TelemetryPoint = z.infer<typeof telemetryPointSchema>;
export type StreamEvent = z.infer<typeof streamEventSchema>;
export type CursorVector = z.infer<typeof cursorVectorSchema>;
export type DashboardSnapshot = z.infer<typeof snapshotSchema>;

export function eventKey(event: StreamEvent): string {
  return `${event.stream}:${event.subject_id}:${event.seq}`;
}

export function mergeEvents(
  current: StreamEvent[],
  incoming: StreamEvent[],
): StreamEvent[] {
  const byKey = new Map(current.map((event) => [eventKey(event), event]));
  for (const event of incoming) byKey.set(eventKey(event), event);
  return [...byKey.values()].sort((a, b) => {
    const at = a.timestamp_epoch ?? Number.MAX_SAFE_INTEGER;
    const bt = b.timestamp_epoch ?? Number.MAX_SAFE_INTEGER;
    return at - bt || a.stream.localeCompare(b.stream) || a.seq - b.seq;
  });
}

export function findCursorGaps(events: StreamEvent[]): string[] {
  const streams = new Map<string, number[]>();
  for (const event of events) {
    const key = `${event.stream}:${event.subject_id}`;
    const list = streams.get(key) ?? [];
    list.push(event.seq);
    streams.set(key, list);
  }

  const gaps: string[] = [];
  for (const [key, sequence] of streams) {
    const sorted = [...new Set(sequence)].sort((a, b) => a - b);
    for (let index = 1; index < sorted.length; index += 1) {
      if (sorted[index] !== sorted[index - 1] + 1) {
        gaps.push(`${key}:${sorted[index - 1] + 1}-${sorted[index] - 1}`);
      }
    }
  }
  return gaps;
}
