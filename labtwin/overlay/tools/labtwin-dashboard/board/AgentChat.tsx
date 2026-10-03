"use client";

import { useEffect, useRef, useState } from "react";
import { portableUuid } from "../src/crypto/portable";
import { apiRequest, ManagementApiError } from "./api";
import { AgentOperations } from "./AgentOperations";

type ChatMessage = { role: "user" | "assistant" | "system"; content: string; request_id?: string };
type ChatRequest = { request_id: string; content: string; reply: string; error: string;
  state: "QUEUED" | "RUNNING" | "SUCCEEDED" | "FAILED" | "UNCERTAIN" | "UNDELIVERED";
  ordinal: number; remaining_seconds: number; observedAt?: number };
const LOCAL_PENDING = "labtwin.portal.pending.v1";
const busy = (item: ChatRequest) => (item.state === "QUEUED" || item.state === "RUNNING") &&
  Date.now() < (item.observedAt ?? Date.now()) + item.remaining_seconds * 1000;
const unknown = (item: ChatRequest) => item.state === "UNCERTAIN" || item.state === "UNDELIVERED" ||
  ((item.state === "QUEUED" || item.state === "RUNNING") && !busy(item));
function savedPending(): ChatRequest | null {
  try {
    const item = JSON.parse(sessionStorage.getItem(LOCAL_PENDING) ?? "null") as ChatRequest | null;
    return item && /^[a-f0-9]{24}$/.test(item.request_id) && typeof item.content === "string" ?
      { ...item, state: "UNDELIVERED", remaining_seconds: 0 } : null;
  } catch { return null; }
}

export function AgentChat({ authenticated, onNotice, onExperimentChanged }: {
  authenticated: boolean; onNotice: (message: string) => void; onExperimentChanged: () => Promise<void>;
}) {
  const [messages, setMessages] = useState<ChatMessage[]>([]);
  const [requests, setRequests] = useState<ChatRequest[]>([]);
  const [input, setInput] = useState("");
  const [connection, setConnection] = useState<"connecting" | "online" | "offline">("offline");
  const [ready, setReady] = useState(false);
  const [posting, setPosting] = useState(false);
  const [error, setError] = useState("");
  const [acknowledged, setAcknowledged] = useState(new Set<string>());
  const [, tick] = useState(0);
  const callbacks = useRef({ onNotice, onExperimentChanged });
  const reconcileRef = useRef<() => Promise<void>>(async () => {});
  const generation = useRef(0);
  const queryVersion = useRef(0);
  const completed = useRef(new Set<string>());
  useEffect(() => { callbacks.current = { onNotice, onExperimentChanged }; }, [onNotice, onExperimentChanged]);

  useEffect(() => {
    const version = ++generation.current;
    if (!authenticated) {
      sessionStorage.removeItem(LOCAL_PENDING);
      setMessages([]); setRequests([]); setAcknowledged(new Set()); setReady(false); setPosting(false); setConnection("offline");
      return;
    }
    let active = true, polling = false;
    let socket: WebSocket | null = null;
    let reconnect: ReturnType<typeof setTimeout> | undefined;
    let opening: ReturnType<typeof setTimeout> | undefined;
    const controller = new AbortController();
    const pending = savedPending();
    if (pending) setRequests([pending]);
    const reconcile = async () => {
      if (!active || polling) return;
      polling = true;
      const query = queryVersion.current;
      try {
        const [history, ledger] = await Promise.all([
          apiRequest<{ messages: ChatMessage[] }>("/api/v2/agent/history", { signal: controller.signal }),
          apiRequest<{ requests: ChatRequest[] }>("/api/v2/agent/requests", { signal: controller.signal }),
        ]);
        if (!active || query !== queryVersion.current) return;
        const now = Date.now();
        const restored = (ledger.data?.requests ?? []).map((item) => ({ ...item, observedAt: now }));
        const local = savedPending();
        if (local && !restored.some((item) => item.request_id === local.request_id)) restored.push(local);
        else if (local) sessionStorage.removeItem(LOCAL_PENDING);
        restored.sort((a, b) => a.ordinal - b.ordinal);
        setMessages(history.data?.messages ?? []); setRequests(restored);
        setReady(true); setError("");
        for (const item of restored) if (item.state === "SUCCEEDED" && !completed.current.has(item.request_id)) {
          completed.current.add(item.request_id);
          if (completed.current.size > 64) completed.current.delete(completed.current.values().next().value!);
          void callbacks.current.onExperimentChanged().catch(() => {});
        }
      } catch (cause) {
        if (active) setError(cause instanceof Error ? cause.message : "无法查询板端聊天状态");
      } finally { polling = false; }
    };
    reconcileRef.current = reconcile;
    const connect = () => {
      if (!active) return;
      setConnection("connecting");
      const current = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/agent`, ["labtwin.agent.v1"]);
      socket = current;
      opening = setTimeout(() => { if (active && current.readyState !== WebSocket.OPEN) { setConnection("offline"); current.close(); } }, 10_000);
      current.onopen = () => { clearTimeout(opening); if (active) { setConnection("online"); void reconcile(); } };
      current.onmessage = (event) => {
        if (!active) return;
        try {
          const payload = JSON.parse(String(event.data)) as { type?: string; request_id?: string };
          if (payload.type === "pending_operation") window.dispatchEvent(new Event("labtwin-agent-operations"));
          /* Frames are hints only: never assign a late reply to the newest text. */
          if (payload.request_id) void reconcile();
        } catch { /* Authoritative polling also recovers malformed/lost frames. */ }
      };
      current.onerror = () => { if (active) setConnection("offline"); };
      current.onclose = () => {
        clearTimeout(opening);
        if (active) { setConnection("offline"); reconnect = setTimeout(connect, 2_000); }
      };
    };
    void reconcile(); connect();
    const interval = setInterval(() => { tick((value) => value + 1); void reconcile(); }, 2_000);
    return () => {
      active = false; controller.abort(); clearInterval(interval); clearTimeout(reconnect); clearTimeout(opening);
      socket?.close(); reconcileRef.current = async () => {};
      generation.current = version + 1;
    };
  }, [authenticated]);

  async function submit(item: ChatRequest) {
    const version = generation.current;
    queryVersion.current++;
    setPosting(true); setError("");
    /* Persist before POST; an ambiguous network failure must reuse this id. */
    try { sessionStorage.setItem(LOCAL_PENDING, JSON.stringify(item)); }
    catch { setPosting(false); setError("浏览器暂存不可用，请启用会话存储后重试；消息未发送。"); return; }
    setRequests((current) => [...current.filter((entry) => entry.request_id !== item.request_id), item]);
    try {
      const result = await apiRequest<ChatRequest>("/api/v2/agent/requests", {
        method: "POST", body: JSON.stringify({ request_id: item.request_id, content: item.content }),
      });
      if (generation.current !== version) return;
      if (!result.data) throw new Error("板端没有返回请求状态，请检查原请求。");
      queryVersion.current++;
      sessionStorage.removeItem(LOCAL_PENDING);
      const accepted = { ...result.data, observedAt: Date.now() };
      setRequests((current) => [...current.filter((entry) => entry.request_id !== item.request_id), accepted]);
      setInput("");
      void reconcileRef.current();
    } catch (cause) {
      if (generation.current !== version) return;
      setError(cause instanceof Error ? cause.message : "发送状态未知，请先检查状态。");
      if (cause instanceof ManagementApiError && cause.status >= 400 && cause.status < 500 && cause.status !== 408) {
        sessionStorage.removeItem(LOCAL_PENDING);
        setRequests((current) => current.map((entry) => entry.request_id === item.request_id ? { ...entry, state: "UNCERTAIN", error: cause.code } : entry));
      }
      void reconcileRef.current();
    } finally { if (generation.current === version) setPosting(false); }
  }
  async function clear() {
    if (!window.confirm("确认清空网页聊天记录？不影响板端语音会话，请求去重记录仍保留。")) return;
    const version = generation.current;
    queryVersion.current++;
    setPosting(true);
    try {
      await apiRequest("/api/v2/agent/history", { method: "DELETE" });
      if (generation.current !== version) return;
      queryVersion.current++;
      sessionStorage.removeItem(LOCAL_PENDING); setMessages([]); setRequests([]);
      callbacks.current.onNotice("网页聊天记录已清空"); void reconcileRef.current();
    } catch (cause) { if (generation.current === version) setError(cause instanceof Error ? cause.message : "清空失败"); }
    finally { if (generation.current === version) setPosting(false); }
  }
  const working = requests.some(busy);
  const uncertain = requests.filter((item) => unknown(item) && !acknowledged.has(item.request_id));
  const transcript = [...messages];
  for (const item of requests) {
    if (item.content && !transcript.some((message) => message.role === "user" && message.request_id === item.request_id))
      transcript.push({ role: "user", content: item.content, request_id: item.request_id });
    if (item.reply && !transcript.some((message) => message.role === "assistant" && message.request_id === item.request_id))
      transcript.push({ role: "assistant", content: item.reply, request_id: item.request_id });
  }
  if (!authenticated) return <section className="single-view"><div className="empty-block"><strong>登录后可与板端 Agent 对话</strong><p>网页聊天独立保存，不会混入板端语音对话。</p></div></section>;
  return <section className="agent-chat">
    <AgentOperations authenticated={authenticated} onNotice={onNotice} onExperimentChanged={onExperimentChanged} />
    <header className="single-view-header"><div><p className="section-kicker">Board Agent</p><h2>与实验台 Agent 对话</h2><p>高影响动作需确认；断线后按原请求编号恢复，不会自动重做操作。</p></div><div className="button-row"><span className={`chat-status ${working ? "working" : connection}`}>{working ? "正在处理" : connection === "online" ? "已连接" : connection === "connecting" ? "连接中" : ready ? "实时连接离线 · 可查询状态" : "已离线"}</span><button className="secondary" disabled={working || posting || uncertain.length > 0} onClick={() => void clear()}>清空记录</button></div></header>
    {error ? <p role="alert">{error}</p> : null}
    {uncertain.map((item) => <div className="empty-block" key={item.request_id} role="status"><strong>请求结果尚未确认</strong><p>{item.state === "UNDELIVERED" ? "未收到发送回执，可能已经受理。检查状态或使用原编号重试，不会重复入队。" : "处理已超时或中断；可能已有操作生效。请先查看实验任务，勿盲目重发。"} 请求 {item.request_id}</p><div className="button-row"><button className="secondary" onClick={() => void reconcileRef.current()}>检查状态</button>{item.state === "UNDELIVERED" ? <button disabled={posting} onClick={() => void submit(item)}>原编号重试</button> : <button className="secondary" onClick={() => setAcknowledged((current) => new Set([...current, item.request_id]))}>已核对任务，继续新对话</button>}</div></div>)}
    {requests.filter((item) => item.state === "FAILED").map((item) => <p role="status" key={item.request_id}>请求失败（{item.error || "AGENT_ERROR"}）；不会自动重复执行。</p>)}
    <div className="chat-transcript" aria-live="polite">{transcript.length ? transcript.map((message, index) => <article key={`${message.role}-${message.request_id ?? index}`} className={`chat-bubble ${message.role}`}><span>{message.role === "user" ? "你" : message.role === "assistant" ? "Agent" : "系统"}</span><p>{message.content}</p></article>) : <div className="chat-empty"><strong>从一个实验问题开始</strong><p>例如：“创建明天上午的 pH 测定任务，分为校准、测量、记录三个步骤”。</p></div>}{working ? <article className="chat-bubble assistant pending"><span>Agent</span><p>正在分析板端状态… 最长等待两分钟，超时后先核对操作结果。</p></article> : null}</div>
    <form className="chat-composer" onSubmit={(event) => {
      event.preventDefault(); const text = input.trim();
      if (!text || !ready || working || posting || uncertain.length) return;
      if (new TextEncoder().encode(text).byteLength > 1024) { setError("消息最长 1024 字节，请缩短后发送。"); return; }
      void submit({ request_id: portableUuid().replaceAll("-", "").slice(0, 24), content: text, reply: "", error: "", state: "UNDELIVERED", ordinal: Date.now(), remaining_seconds: 0 });
    }}><textarea aria-label="消息内容" value={input} maxLength={1024} rows={3} placeholder="向实验台 Agent 发送消息…" onChange={(event) => setInput(event.target.value)} /><button disabled={!input.trim() || !ready || working || posting || uncertain.length > 0}>发送</button></form>
  </section>;
}
