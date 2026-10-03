import type { DashboardSnapshot, StreamEvent } from "../protocol/schema";

const now = 1784120400;

function telemetry() {
  return Array.from({ length: 36 }, (_, index) => {
    const temperature = 26.1 + index * 0.1 + Math.sin(index / 3) * 0.45;
    return {
      seq: index + 1,
      timestamp_epoch: now - (35 - index) * 30,
      temperature_avg: Number(temperature.toFixed(2)),
      humidity_avg: Number((62 + Math.cos(index / 4) * 2.3).toFixed(2)),
      temperature_min: Number((temperature - 0.12).toFixed(2)),
      temperature_max: Number((temperature + 0.12).toFixed(2)),
      humidity_min: 59.4,
      humidity_max: 65.1,
      valid_samples: 30,
      stale_samples: index === 13 ? 2 : 0,
    };
  });
}

const events: StreamEvent[] = [
  {
    stream: "experiment",
    subject_id: "M12",
    seq: 1,
    event_id: "00000001-11a7",
    timestamp_epoch: now - 1960,
    uptime_ms: 220100,
    event_type: "EXPERIMENT_CREATED",
    source: "cli",
    payload: { state: "READY" },
  },
  {
    stream: "experiment",
    subject_id: "M12",
    seq: 2,
    event_id: "00000002-24c8",
    timestamp_epoch: now - 1870,
    uptime_ms: 310100,
    event_type: "EXPERIMENT_STARTED",
    source: "voice",
    payload: { state: "RUNNING" },
  },
  {
    stream: "experiment",
    subject_id: "M12",
    seq: 3,
    event_id: "00000003-30d2",
    timestamp_epoch: now - 720,
    uptime_ms: 1460100,
    event_type: "OBSERVATION_ADDED",
    source: "voice",
    payload: { text: "样品颜色略微加深，没有明显气泡。" },
  },
  {
    stream: "environment",
    subject_id: "global",
    seq: 1,
    event_id: "ENV-00000001-CREATED",
    timestamp_epoch: now - 410,
    uptime_ms: 1770100,
    event_type: "TEMP_HIGH_CREATED",
    source: "sensor",
    payload: { event_id: "ENV-00000001", value: 30.08, threshold: 30 },
  },
  {
    stream: "environment",
    subject_id: "global",
    seq: 2,
    event_id: "ENV-00000001-ACK",
    timestamp_epoch: now - 315,
    uptime_ms: 1865100,
    event_type: "TEMP_HIGH_ACKNOWLEDGED",
    source: "touch",
    payload: { event_id: "ENV-00000001" },
  },
];

export const demoSnapshot: DashboardSnapshot = {
  device: {
    device_id: "gemini-s1-lab-01",
    boot_id: "b-1784117000-4f92",
    firmware: "m4-environment-temp-fix / 45d0e9b4",
    protocol: "labtwin.dashboard.v1",
    clock_trusted: true,
    storage_error: false,
    sampled_epoch: now,
    sensor_stale: false,
  },
  experiments: [
    {
      schema_version: 1,
      experiment_id: "M12",
      name: "样品稳定性测试",
      state: "RUNNING",
      current_step: 1,
      steps: [
        { title: "样品预处理", completed: true },
        { title: "恒温观察", completed: false },
        { title: "结果复核", completed: false },
      ],
      timers: [
        {
          timer_id: "T-01",
          label: "恒温观察",
          step_index: 1,
          state: "RUNNING",
          remaining_seconds: 1122,
          due_epoch: now + 1122,
        },
      ],
      last_observation: "样品颜色略微加深，没有明显气泡。",
      last_event_seq: 3,
      updated_epoch: now - 720,
    },
    {
      schema_version: 1,
      experiment_id: "M15",
      name: "缓冲液耐久性复测",
      state: "PAUSED",
      current_step: 0,
      steps: [
        { title: "称量与配制", completed: false },
        { title: "静置复测", completed: false },
      ],
      timers: [],
      last_observation: "等待第二批试剂。",
      last_event_seq: 2,
      updated_epoch: now - 1280,
    },
    {
      schema_version: 1,
      experiment_id: "M09",
      name: "空白对照记录",
      state: "COMPLETED",
      current_step: 1,
      steps: [
        { title: "空白采集", completed: true },
        { title: "记录归档", completed: true },
      ],
      timers: [],
      last_observation: "空白对照无异常。",
      last_event_seq: 7,
      updated_epoch: now - 6420,
    },
  ],
  environment_events: [
    {
      event_id: "ENV-00000001",
      rule_id: "TEMP_HIGH",
      severity: "WARNING",
      state: "ACKNOWLEDGED",
      sensor: "temperature",
      measured_value: 30.08,
      trigger_threshold: 30,
      clear_threshold: 28,
      created_epoch: now - 410,
      acknowledged_epoch: now - 315,
      recovered_epoch: null,
      ack_source: "touch",
      experiment_id: "M12",
    },
  ],
  telemetry: telemetry(),
  events,
  cursor: {
    boot_id: "b-1784117000-4f92",
    experiments: { M12: 3, M15: 2, M09: 7 },
    environment: 2,
    telemetry: 36,
  },
};
