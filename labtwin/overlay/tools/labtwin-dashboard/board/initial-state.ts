import { demoSnapshot } from "../src/data/demo";
import type { DashboardSnapshot } from "../src/protocol/schema";

export const boardViews = ["overview", "experiments", "calendar", "agent", "environment",
  "reports", "diagnostics", "network", "device", "ai", "skills", "logs", "maintenance"] as const;
export type BoardView = typeof boardViews[number];

export function previewHost(hostname: string) {
  return hostname === "localhost" || hostname === "127.0.0.1";
}

export function initialSnapshot(preview: boolean): DashboardSnapshot {
  if (preview) return demoSnapshot;
  return {
    device: { device_id: "", boot_id: "", firmware: "等待板卡数据", protocol: "labtwin.dashboard.v1",
      clock_trusted: false, storage_error: false, sampled_epoch: null, sensor_stale: true },
    experiments: [], environment_events: [], telemetry: [], events: [],
    cursor: { boot_id: "", experiments: {}, environment: 0, telemetry: 0 },
  };
}

export function navigationFromHash(hash: string): { view: BoardView; selectedId: string } {
  const params = new URLSearchParams(hash.replace(/^#/, ""));
  const view = params.get("view");
  return { view: boardViews.includes(view as BoardView) ? view as BoardView : "overview",
    selectedId: params.get("id") ?? "" };
}

export function canRestoreCachedSnapshot(cached: DashboardSnapshot,
  identity: { device_id: string; boot_id: string } | null, liveSeen: boolean) {
  return !liveSeen && !!identity?.device_id && !!identity.boot_id &&
    cached.device.device_id === identity.device_id && cached.device.boot_id === identity.boot_id;
}
