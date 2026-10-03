"use client";
import { useEffect, useRef, useState } from "react";
import { apiRequest, postJson } from "./api";

type Operation = {
  id: string; experiment_id: string; action: string; state: string;
  token?: string; remaining_seconds?: number; password_required?: boolean; result?: string;
};
const labels: Record<string, string> = { complete: "完成", cancel: "取消", delete: "删除" };
export function AgentOperations({ authenticated, onNotice, onExperimentChanged }: {
  authenticated: boolean; onNotice: (text: string) => void; onExperimentChanged: () => Promise<void>;
}) {
  const [operations, setOperations] = useState<Operation[]>([]);
  const [selected, setSelected] = useState<string | null>(null);
  const [confirmId, setConfirmId] = useState("");
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [tlsWarning, setTlsWarning] = useState("");
  const callbacks = useRef({ onNotice, onExperimentChanged });
  const previous = useRef(new Map<string, string>());
  const refreshRef = useRef<() => Promise<void>>(async () => {});
  useEffect(() => { callbacks.current = { onNotice, onExperimentChanged }; }, [onNotice, onExperimentChanged]);
  useEffect(() => {
    if (!authenticated) return;
    let active = true;
    let inflight = false;
    const refresh = async () => {
      if (inflight) return;
      inflight = true;
      try {
        const response = await apiRequest<{ operations: Operation[] }>("/api/v2/agent/operations");
        if (!active) return;
        const next = response.data?.operations ?? [];
        const changed = next.some((op) => previous.current.has(op.id) && previous.current.get(op.id) !== op.state && op.state !== "PENDING");
        next.forEach((op) => previous.current.set(op.id, op.state));
        setOperations(next); setError("");
        if (changed) void callbacks.current.onExperimentChanged();
        const status = await apiRequest<{ tls_status?: string }>("/api/v2/status");
        if (active) setTlsWarning(status.data?.tls_status === "TLS_TIME_UNSYNCED" ? "板端时间尚未同步，云端对话与语音暂不可用。" :
          status.data?.tls_status === "TLS_TRUST_UNAVAILABLE" ? "板端证书信任根不可用，云端连接已阻止。" : "");
      } catch (cause) {
        if (active) setError(cause instanceof Error ? cause.message : "无法读取待确认操作");
      } finally { inflight = false; }
    };
    refreshRef.current = refresh;
    const notification = () => { void refresh(); };
    window.addEventListener("labtwin-agent-operations", notification);
    const timer = window.setInterval(notification, 2_000);
    notification();
    return () => { active = false; window.clearInterval(timer); window.removeEventListener("labtwin-agent-operations", notification); };
  }, [authenticated]);
  const operation = operations.find((item) => item.id === selected);
  async function resolve(op: Operation, cancel: boolean) {
    setBusy(true); setError("");
    try {
      const response = await postJson<Operation>(`/api/v2/agent/operations/${encodeURIComponent(op.id)}/${cancel ? "cancel" : "confirm"}`, {
        token: op.token, ...(op.action === "delete" && !cancel ? { confirm_experiment_id: confirmId, ...(op.password_required ? { password } : {}) } : {}),
      });
      const state = response.data?.state;
      callbacks.current.onNotice(state === "APPLIED" ? "操作已提交；快照若需修复将由板端自动处理" : state === "CANCELLED" ? "已取消待确认操作" :
        state === "UNCERTAIN" ? "结果不确定，可能已执行；请检查任务，勿重复操作" : state === "EXPIRED" ? "操作已过期，请重新提议" : response.data?.result || "操作未执行");
      setSelected(null); setPassword(""); setConfirmId("");
      await refreshRef.current();
      await callbacks.current.onExperimentChanged();
    } catch (cause) { setError(cause instanceof Error ? cause.message : "确认失败"); }
    finally { setBusy(false); }
  }
  if (!authenticated) return null;
  const pending = operations.filter((op) => op.state === "PENDING");
  const uncertain = operations.filter((op) => op.state === "UNCERTAIN");
  return <section className="agent-operations" aria-label="Agent 操作确认">
    {tlsWarning ? <p role="status">{tlsWarning}</p> : null}
    {error ? <p role="alert">{error}</p> : null}
    {uncertain.map((op) => <p role="alert" key={op.id}>{op.experiment_id}：{labels[op.action]}结果不确定，请核查任务，不能重复确认。</p>)}
    {pending.map((op) => <div className="operation-card" key={op.id}>
      <span>待确认：{labels[op.action]} {op.experiment_id}（剩余 {op.remaining_seconds ?? 0} 秒，尚未执行）</span>
      <button disabled={busy} onClick={() => { setSelected(op.id); setConfirmId(""); setPassword(""); }}>确认{labels[op.action]} {op.experiment_id}</button>
      <button className="secondary" disabled={busy} onClick={() => void resolve(op, true)}>放弃此操作</button>
    </div>)}
    {operation ? <div className="operation-dialog" role="dialog" aria-modal="true" aria-label="确认 Agent 操作">
      <h3>{labels[operation.action]}实验 {operation.experiment_id}</h3>
      <p>{operation.state === "PENDING" ? `剩余 ${operation.remaining_seconds ?? 0} 秒。确认前板端将再次检查任务状态。` : "操作已结束或过期，不能执行。"}</p>
      {operation.action === "delete" ? <><p>删除不可恢复，请输入完整实验编号。</p><label>完整实验编号<input value={confirmId} onChange={(event) => setConfirmId(event.target.value)} /></label>
        {operation.password_required ? <label>管理员密码<input type="password" autoComplete="current-password" value={password} onChange={(event) => setPassword(event.target.value)} /></label> : null}</> : null}
      <div className="button-row"><button disabled={busy || operation.state !== "PENDING" || (operation.action === "delete" && (confirmId !== operation.experiment_id || (operation.password_required && !password)))} onClick={() => void resolve(operation, false)}>确认执行</button>
        <button className="secondary" disabled={busy} onClick={() => { setSelected(null); setPassword(""); }}>暂不确认</button></div>
    </div> : null}
  </section>;
}
