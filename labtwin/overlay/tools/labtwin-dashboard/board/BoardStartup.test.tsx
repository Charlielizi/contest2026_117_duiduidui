// @vitest-environment jsdom
import React from "react";
import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { BoardAdminApp } from "./BoardAdminApp";
import { apiRequest, postJson } from "./api";
import { initialSnapshot } from "./initial-state";

vi.mock("./api", async (original) => ({ ...await original<object>(), apiRequest: vi.fn(), postJson: vi.fn() }));
vi.mock("../app/TelemetryChart", () => ({ TelemetryChart: () => null }));
vi.mock("./AgentChat", () => ({ AgentChat: () => <p>聊天视图验收</p> }));
vi.mock("../src/db/labtwin-db", () => ({ clearSnapshotCache: vi.fn(), loadLatestSnapshot: vi.fn(), saveSnapshot: vi.fn() }));
vi.mock("../src/transport/dashboard-client", () => ({ DashboardClient: class { connect() {} disconnect() {} } }));
const envelope = (data: unknown) => ({ request_id: "test", revision: 1, error: null, data });
beforeEach(() => {
  const realLocation = window.location;
  vi.stubGlobal("location", { hostname: "192.168.1.41", host: "192.168.1.41", protocol: "http:", get hash() { return realLocation.hash; } });
  window.matchMedia = vi.fn().mockReturnValue({ matches: false });
  window.history.replaceState(null, "", "#view=overview");
  vi.mocked(apiRequest).mockImplementation(() => new Promise(() => {}));
});
afterEach(() => { cleanup(); vi.unstubAllGlobals(); vi.clearAllMocks(); });

describe("board initial render and authentication reset", () => {
  it("never shows sample records while board authentication/data are pending", () => {
    render(<BoardAdminApp />);
    expect(screen.queryByText("样品稳定性测试")).toBeNull();
    expect(screen.queryByText("M12")).toBeNull();
    expect(screen.queryByText("m4-environment-temp-fix / 45d0e9b4")).toBeNull();
    expect(screen.getByText("等待板卡数据")).toBeTruthy();
    expect(screen.getByText("0 个实验已载入")).toBeTruthy();
  });
  it("keeps the requested chat route immediately and after startup effects", async () => {
    window.history.replaceState(null, "", "#view=agent");
    render(<BoardAdminApp />);
    expect(screen.getByText("聊天视图验收")).toBeTruthy();
    await waitFor(() => expect(window.location.hash).toBe("#view=agent"));
  });
  it("does not populate samples after logging out", async () => {
    let authenticated = true;
    vi.mocked(apiRequest).mockImplementation(async (path) => {
      if (path === "/api/v2/auth/state") return envelope({ initialized: true, authenticated, role: authenticated ? "admin" : "guest" });
      if (path === "/api/v2/labtwin/snapshot") return envelope(initialSnapshot(false));
      if (path === "/api/v2/status") return envelope({ device_id: "real-board", boot_id: "real-boot", hostname: "board", wifi: { ip: "192.168.1.41", ssid: "test" }, service: { state: "ready" } });
      return envelope({});
    });
    vi.mocked(postJson).mockImplementation(async () => { authenticated = false; return envelope({}); });
    render(<BoardAdminApp />);
    await waitFor(() => expect(screen.getByRole("button", { name: "管理员", exact: true })).toBeTruthy());
    fireEvent.click(screen.getByRole("button", { name: "设备设置", exact: true }));
    fireEvent.click(screen.getByRole("button", { name: "退出管理员" }));
    await waitFor(() => expect(screen.getByRole("button", { name: "管理员登录" })).toBeTruthy());
    expect(screen.queryByText("样品稳定性测试")).toBeNull();
    expect(screen.getByText("0 个实验已载入")).toBeTruthy();
  });
});
