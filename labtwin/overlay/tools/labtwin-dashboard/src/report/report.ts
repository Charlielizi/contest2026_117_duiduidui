import type {
  DashboardSnapshot,
  EnvironmentEvent,
  Experiment,
  StreamEvent,
  TelemetryPoint,
} from "../protocol/schema";
import { findCursorGaps } from "../protocol/schema";

/** 确定性 JSON：递归排序 key、过滤 undefined、无空格。digest 与导出文件共用此字节序列。 */
export function canonicalize(value: unknown): string {
  if (value === null || value === undefined) return "null";
  if (typeof value !== "object") return JSON.stringify(value);
  if (Array.isArray(value)) return `[${value.map(canonicalize).join(",")}]`;
  const obj = value as Record<string, unknown>;
  const keys = Object.keys(obj).filter((k) => obj[k] !== undefined).sort();
  return `{${keys.map((k) => `${JSON.stringify(k)}:${canonicalize(obj[k])}`).join(",")}}`;
}

export type ReportBundle = {
  markdown: string;
  evidence: {
    schema_version: 1;
    generated_at: string;
    device: DashboardSnapshot["device"];
    experiment: Experiment;
    events: StreamEvent[];
    environment_events: EnvironmentEvent[];
    telemetry: TelemetryPoint[];
    cursor: DashboardSnapshot["cursor"];
    integrity: {
      complete: boolean;
      cursor_gaps: string[];
      clock_trusted: boolean;
      storage_error: boolean;
    };
  };
  digest: string;
};

function time(epoch: number | null) {
  return epoch
    ? new Intl.DateTimeFormat("zh-CN", {
        dateStyle: "medium",
        timeStyle: "medium",
        hour12: false,
      }).format(epoch * 1000)
    : "板端绝对时间不可信";
}

function eventRef(event: StreamEvent) {
  return `[E:${event.stream}/${event.subject_id}/${event.seq}]`;
}

export async function buildReport(
  snapshot: DashboardSnapshot,
  experimentId: string,
): Promise<ReportBundle> {
  const experiment = snapshot.experiments.find(
    (item) => item.experiment_id === experimentId,
  );
  if (!experiment) throw new Error(`实验 ${experimentId} 不存在`);

  const events = snapshot.events.filter(
    (event) =>
      (event.stream === "experiment" && event.subject_id === experimentId) ||
      (event.stream === "environment" &&
        snapshot.environment_events.some(
          (environment) =>
            environment.experiment_id === experimentId &&
            environment.event_id === event.payload.event_id,
        )),
  );
  const environmentEvents = snapshot.environment_events.filter(
    (event) => event.experiment_id === experimentId,
  );
  const eventTimes = events
    .map((event) => event.timestamp_epoch)
    .filter((value): value is number => value != null);
  const startTime = eventTimes.length ? Math.min(...eventTimes) : null;
  const endTime = eventTimes.length ? Math.max(...eventTimes) : null;
  const telemetry = (startTime != null && endTime != null)
    ? snapshot.telemetry.filter((point) => point.timestamp_epoch >= startTime && point.timestamp_epoch <= endTime)
    : snapshot.telemetry;
  const gaps = findCursorGaps(events);
  const complete =
    events.length > 0 &&
    gaps.length === 0 &&
    snapshot.device.clock_trusted &&
    !snapshot.device.storage_error;

  const generatedAt = new Date().toISOString();
  const evidence = {
    schema_version: 1 as const,
    generated_at: generatedAt,
    device: snapshot.device,
    experiment,
    events,
    environment_events: environmentEvents,
    telemetry,
    cursor: snapshot.cursor,
    integrity: {
      complete,
      cursor_gaps: gaps,
      clock_trusted: snapshot.device.clock_trusted,
      storage_error: snapshot.device.storage_error,
    },
  };

  const digest = sha256Hex(canonicalize(evidence));

  const lines = [
    `# LabTwin ${experiment.experiment_id} ${experiment.state === "COMPLETED" ? "实验报告" : "阶段报告"}`,
    "",
    `- 实验名称：${experiment.name}`,
    `- 当前状态：${experiment.state}`,
    `- 设备：${snapshot.device.device_id}`,
    `- 固件：${snapshot.device.firmware}`,
    `- 生成时间：${generatedAt}`,
    `- 证据完整性：${complete ? "完整" : "存在保留项"}`,
    "",
    "## 执行时间线",
    "",
    ...events.map(
      (event) =>
        `- ${time(event.timestamp_epoch)}：${event.event_type}，来源 ${event.source}。${eventRef(event)}`,
    ),
    "",
    "## 步骤与观察",
    "",
    ...experiment.steps.map(
      (step, index) =>
        `- ${index + 1}. ${step.title}：${step.completed ? "已完成" : "未完成"}`,
    ),
    `- 最近观察：${experiment.last_observation || "无"}`,
    "",
    "## 环境事件",
    "",
    ...(environmentEvents.length
      ? environmentEvents.map(
          (event) =>
            `- ${event.rule_id}：${event.measured_value}，状态 ${event.state}，事件 ${event.event_id}。`,
        )
      : ["- 无已关联环境事件。"]),
    "",
    "## 数据完整性",
    "",
    `- 板端时钟：${snapshot.device.clock_trusted ? "可信" : "不可信"}`,
    `- 板端存储：${snapshot.device.storage_error ? "异常" : "正常"}`,
    `- 事件序号：${gaps.length ? `存在空洞 ${gaps.join("、")}` : "连续"}`,
    `- Evidence SHA-256：${digest}`,
    "",
    "> 本报告由确定性模板生成。事实来源为随附 evidence.json，不包含未经引用的 AI 推断。",
  ];

  return { markdown: lines.join("\n"), evidence, digest };
}

export function downloadText(filename: string, content: string, type: string) {
  const url = URL.createObjectURL(new Blob([content], { type }));
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = filename;
  anchor.click();
  URL.revokeObjectURL(url);
}
import { sha256Hex } from "../crypto/portable";
