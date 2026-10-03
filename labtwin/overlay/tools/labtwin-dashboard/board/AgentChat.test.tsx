// @vitest-environment jsdom
import React from "react";
import { act, cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { AgentChat } from "./AgentChat";
import { apiRequest } from "./api";
vi.mock("./api", async (original) => ({ ...await original<object>(), apiRequest: vi.fn() }));
vi.mock("./AgentOperations", () => ({ AgentOperations: () => null }));
let counter = 0;
vi.mock("../src/crypto/portable", () => ({ portableUuid: () => (++counter).toString(16).padStart(24, "0") }));
type Request = { request_id: string; content: string; reply: string; state: string; error: string; ordinal: number; remaining_seconds: number };
let requests: Request[], history: object[], posts: Request[], loseReceipt: boolean, failReads: boolean;
class Socket {
  static OPEN = 1; static all: Socket[] = [];
  readyState = 0; onopen?: () => void; onclose?: () => void; onerror?: () => void; onmessage?: (event: { data: string }) => void;
  constructor() { Socket.all.push(this); }
  close() { this.readyState = 3; this.onclose?.(); }
  open() { this.readyState = 1; this.onopen?.(); }
}
const envelope = (data: unknown) => ({ request_id: "http", revision: 1, error: null, data });
beforeEach(() => {
  counter = 0; requests = []; history = []; posts = []; loseReceipt = false; failReads = false;
  Socket.all = []; sessionStorage.clear(); vi.stubGlobal("WebSocket", Socket);
  vi.mocked(apiRequest).mockImplementation(async (path, options) => {
    if (options?.method === "POST") {
      const body = JSON.parse(String(options.body));
      const item = { ...body, reply: "", state: "QUEUED", error: "", ordinal: counter, remaining_seconds: 120 };
      posts.push(item);
      if (loseReceipt) throw new Error("connection lost");
      requests = [item]; return envelope(item);
    }
    if (failReads) throw new Error("offline");
    return envelope(path.endsWith("/history") ? { messages: history } : { requests });
  });
});
afterEach(() => { cleanup(); vi.useRealTimers(); vi.unstubAllGlobals(); vi.clearAllMocks(); });
const props = () => ({ authenticated: true, onNotice: vi.fn(), onExperimentChanged: vi.fn().mockResolvedValue(undefined) });
async function send(text = "一样的文本") {
  fireEvent.change(screen.getByLabelText("消息内容"), { target: { value: text } });
  await waitFor(() => expect((screen.getByText("发送") as HTMLButtonElement).disabled).toBe(false));
  fireEvent.click(screen.getByText("发送"));
  await waitFor(() => expect(posts.length).toBeGreaterThan(0));
}
describe("request-based portal chat", () => {
  it("restores running requests after refresh and disables clear/send without replay", async () => {
    requests = [{ request_id: "a".repeat(24), content: "创建实验", reply: "", state: "RUNNING", error: "", ordinal: 1, remaining_seconds: 100 }];
    render(<AgentChat {...props()} />);
    expect(await screen.findByText("创建实验")).toBeTruthy();
    expect(screen.getByText("正在处理")).toBeTruthy();
    expect((screen.getByText("清空记录") as HTMLButtonElement).disabled).toBe(true);
    expect(posts).toHaveLength(0);
  });
  it("uses the same id when a POST receipt is lost, and never automatically retries", async () => {
    loseReceipt = true; render(<AgentChat {...props()} />); await send();
    expect(await screen.findByText("原编号重试")).toBeTruthy();
    expect(posts).toHaveLength(1);
    const id = posts[0].request_id;
    loseReceipt = false; fireEvent.click(screen.getByText("原编号重试"));
    await waitFor(() => expect(posts).toHaveLength(2));
    expect(posts[1].request_id).toBe(id);
  });
  it("gives identical messages distinct ids and ignores untracked late frames", async () => {
    render(<AgentChat {...props()} />); await send();
    const first = posts[0].request_id;
    requests[0] = { ...requests[0], state: "SUCCEEDED", reply: "第一次回复" };
    history = [{ role: "user", content: "一样的文本", request_id: first }, { role: "assistant", content: "第一次回复", request_id: first }];
    act(() => Socket.all[0].onmessage?.({ data: JSON.stringify({ type: "response", request_id: first }) }));
    expect(await screen.findByText("第一次回复")).toBeTruthy();
    await send(); await waitFor(() => expect(posts).toHaveLength(2));
    expect(posts[1].request_id).not.toBe(first);
    act(() => Socket.all[0].onmessage?.({ data: JSON.stringify({ type: "response", content: "旧消息假回复" }) }));
    expect(screen.queryByText("旧消息假回复")).toBeNull();
    expect(screen.getByText("正在处理")).toBeTruthy();
  });
  it("recovers completed history without duplicating ledger replies", async () => {
    const id = "b".repeat(24);
    requests = [{ request_id: id, content: "问题", reply: "已完成", state: "SUCCEEDED", error: "", ordinal: 1, remaining_seconds: 0 }];
    history = [{ role: "user", content: "问题", request_id: id }, { role: "assistant", content: "已完成", request_id: id }];
    render(<AgentChat {...props()} />);
    await screen.findByText("已完成"); expect(screen.getAllByText("已完成")).toHaveLength(1);
    expect(screen.getAllByText("问题")).toHaveLength(1); expect(posts).toHaveLength(0);
  });
  it("bounds a hanging socket and a request deadline even when polling is offline", async () => {
    vi.useFakeTimers();
    requests = [{ request_id: "c".repeat(24), content: "计时", reply: "", state: "RUNNING", error: "", ordinal: 1, remaining_seconds: 12 }];
    render(<AgentChat {...props()} />);
    await act(async () => { await Promise.resolve(); await Promise.resolve(); });
    failReads = true;
    await act(async () => { await vi.advanceTimersByTimeAsync(10_000); });
    expect(screen.queryByText("连接中")).toBeNull();
    expect(Socket.all[0].readyState).toBe(3);
    await act(async () => { await vi.advanceTimersByTimeAsync(4_000); });
    expect(screen.queryByText("正在处理")).toBeNull();
    expect(screen.getByText("请求结果尚未确认")).toBeTruthy();
    expect(posts).toHaveLength(0);
    fireEvent.click(screen.getByText("已核对任务，继续新对话"));
    expect(screen.queryByText("请求结果尚未确认")).toBeNull();
  });
  it("cleans reconnect timers and local pending content on logout", async () => {
    const component = render(<AgentChat {...props()} />); await screen.findByText("从一个实验问题开始");
    sessionStorage.setItem("labtwin.portal.pending.v1", "private local message");
    component.rerender(<AgentChat {...props()} authenticated={false} />);
    expect(sessionStorage.getItem("labtwin.portal.pending.v1")).toBeNull();
    expect(Socket.all[0].readyState).toBe(3);
  });
});
