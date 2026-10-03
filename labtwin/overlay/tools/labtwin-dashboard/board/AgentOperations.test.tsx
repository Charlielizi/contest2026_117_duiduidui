// @vitest-environment jsdom
import React from "react";
import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { AgentOperations } from "./AgentOperations";
import { apiRequest, postJson } from "./api";
vi.mock("./api", () => ({ apiRequest: vi.fn(), postJson: vi.fn() }));
const token = "a".repeat(64);
const item = { id: "op-1", experiment_id: "EXP-1", action: "delete", state: "PENDING", token, remaining_seconds: 120 };
let current = { ...item, password_required: false };
beforeEach(() => {
  current = { ...item, password_required: false };
  vi.mocked(apiRequest).mockImplementation(async (path) => ({ request_id: "test", revision: 1,
    error: null, data: path.endsWith("/status") ? { tls_status: "TLS_READY" } : { operations: [current] } }));
  vi.mocked(postJson).mockResolvedValue({ request_id: "test", revision: 1, error: null, data: { ...item, state: "APPLIED" } });
});
afterEach(() => { cleanup(); vi.clearAllMocks(); });
function show() { return render(<AgentOperations authenticated onNotice={vi.fn()} onExperimentChanged={vi.fn().mockResolvedValue(undefined)} />); }
describe("Agent confirmation", () => {
  it("restores pending operations and requires the exact id without private-mode password", async () => {
    show(); fireEvent.click(await screen.findByText("确认删除 EXP-1"));
    expect(screen.queryByLabelText("管理员密码")).toBeNull();
    const button = screen.getByText("确认执行") as HTMLButtonElement;
    expect(button.disabled).toBe(true);
    fireEvent.change(screen.getByLabelText("完整实验编号"), { target: { value: "EXP-1" } });
    fireEvent.click(button);
    await waitFor(() => expect(postJson).toHaveBeenCalledWith("/api/v2/agent/operations/op-1/confirm", { token, confirm_experiment_id: "EXP-1" }));
  });
  it("requires password in public mode", async () => {
    current.password_required = true;
    show(); fireEvent.click(await screen.findByText("确认删除 EXP-1"));
    fireEvent.change(screen.getByLabelText("完整实验编号"), { target: { value: "EXP-1" } });
    expect((screen.getByText("确认执行") as HTMLButtonElement).disabled).toBe(true);
    fireEvent.change(screen.getByLabelText("管理员密码"), { target: { value: "test-only-password" } });
    fireEvent.click(screen.getByText("确认执行"));
    await waitFor(() => expect(postJson).toHaveBeenCalledWith(expect.any(String), { token, confirm_experiment_id: "EXP-1", password: "test-only-password" }));
  });
  it("cancels without executing and preserves confirmation failures", async () => {
    vi.mocked(postJson).mockRejectedValue(new Error("已过期或令牌无效"));
    show(); fireEvent.click(await screen.findByText("放弃此操作"));
    expect(await screen.findByRole("alert")).toHaveProperty("textContent", "已过期或令牌无效");
    expect(postJson).toHaveBeenCalledWith("/api/v2/agent/operations/op-1/cancel", { token });
  });
  it("does not fetch operations before login", () => {
    render(<AgentOperations authenticated={false} onNotice={vi.fn()} onExperimentChanged={vi.fn()} />);
    expect(apiRequest).not.toHaveBeenCalled();
  });
});
