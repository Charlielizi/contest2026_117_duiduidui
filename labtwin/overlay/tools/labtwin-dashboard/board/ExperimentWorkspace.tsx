"use client";

import { useEffect, useMemo, useState } from "react";
import type { Experiment } from "../src/protocol/schema";
import { apiRequest, postJson, putJson } from "./api";
import { calendarDates, experimentInterval, experimentOnDay, moveCalendar } from "./calendar-time";
import { loadExperimentHistory } from "./experiment-history";

type Props = {
  fallback: Experiment[];
  selectedId: string;
  setSelectedId: (id: string) => void;
  editable: boolean;
  privateAdmin?: boolean;
  refreshLive: () => Promise<void>;
  onNotice: (message: string) => void;
};

const stateName: Record<Experiment["state"], string> = {
  READY: "待开始", RUNNING: "进行中", PAUSED: "已暂停",
  COMPLETED: "已完成", CANCELLED: "已取消", RECOVERY_ERROR: "恢复异常",
};

type Draft = { name: string; description: string; plannedStart: string; plannedEnd: string; steps: string[] };
const emptyDraft = (): Draft => ({ name: "", description: "", plannedStart: "", plannedEnd: "", steps: [""] });

function formatTime(epoch: number | null | undefined) {
  return epoch ? new Date(epoch * 1000).toLocaleString("zh-CN", { hour12: false }) : "—";
}

function localInput(epoch: number | null | undefined) {
  if (!epoch) return "";
  const date = new Date(epoch * 1000);
  const offset = date.getTimezoneOffset() * 60_000;
  return new Date(date.getTime() - offset).toISOString().slice(0, 16);
}

function epoch(value: string) {
  if (!value) return null;
  const result = Math.floor(new Date(value).getTime() / 1000);
  return Number.isFinite(result) ? result : null;
}

function draftOf(item: Experiment): Draft {
  return {
    name: item.name,
    description: item.description ?? "",
    plannedStart: localInput(item.planned_start_epoch),
    plannedEnd: localInput(item.planned_end_epoch),
    steps: item.steps.map((step) => step.title),
  };
}

function TaskForm({ draft, setDraft, submitLabel, busy, onSubmit, onCancel }: {
  draft: Draft; setDraft: (next: Draft) => void; submitLabel: string; busy: boolean;
  onSubmit: () => void; onCancel?: () => void;
}) {
  const setStep = (index: number, value: string) => setDraft({ ...draft, steps: draft.steps.map((step, i) => i === index ? value : step) });
  return <form className="task-form form-stack" onSubmit={(event) => { event.preventDefault(); onSubmit(); }}>
    <label>任务名称<input required maxLength={63} value={draft.name} onChange={(event) => setDraft({ ...draft, name: event.target.value })} /></label>
    <label>实验说明<textarea rows={3} maxLength={512} value={draft.description} onChange={(event) => setDraft({ ...draft, description: event.target.value })} /></label>
    <div className="task-time-grid">
      <label>计划开始<input type="datetime-local" value={draft.plannedStart} onChange={(event) => setDraft({ ...draft, plannedStart: event.target.value })} /></label>
      <label>计划结束<input type="datetime-local" value={draft.plannedEnd} min={draft.plannedStart || undefined} onChange={(event) => setDraft({ ...draft, plannedEnd: event.target.value })} /></label>
    </div>
    <fieldset className="task-steps"><legend>实验步骤</legend>{draft.steps.map((step, index) => <div key={index} className="task-step-input"><input required placeholder={`步骤 ${index + 1}`} maxLength={95} value={step} onChange={(event) => setStep(index, event.target.value)} /><button type="button" className="secondary" disabled={busy || draft.steps.length === 1} onClick={() => setDraft({ ...draft, steps: draft.steps.filter((_, i) => i !== index) })}>移除</button></div>)}<button type="button" className="secondary" disabled={busy || draft.steps.length >= 16} onClick={() => setDraft({ ...draft, steps: [...draft.steps, ""] })}>添加步骤</button></fieldset>
    <div className="button-row"><button disabled={busy}>{submitLabel}</button>{onCancel ? <button type="button" className="secondary" disabled={busy} onClick={onCancel}>取消</button> : null}</div>
  </form>;
}

export function ExperimentWorkspace(props: Props) {
  const privateAdmin = props.privateAdmin === true;
  const [items, setItems] = useState<Experiment[]>(props.fallback);
  const [loading, setLoading] = useState(false);
  const [busy, setBusy] = useState(false);
  const [creating, setCreating] = useState(false);
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState<Draft>(emptyDraft);
  const [observation, setObservation] = useState("");
  const [timerLabel, setTimerLabel] = useState("步骤计时");
  const [timerSeconds, setTimerSeconds] = useState(300);

  const selected = useMemo(() => items.find((item) => item.experiment_id === props.selectedId) ?? items[0], [items, props.selectedId]);

  async function reload() {
    setLoading(true);
    try {
      const next = await loadExperimentHistory();
      setItems(next);
      if (next.length && !next.some((item) => item.experiment_id === props.selectedId)) props.setSelectedId(next[0].experiment_id);
    } catch (error) {
      props.onNotice(error instanceof Error ? error.message : "无法读取实验历史");
    } finally { setLoading(false); }
  }

  useEffect(() => {
    if (!props.editable) return;
    const timer = window.setTimeout(() => { void reload(); }, 0);
    return () => window.clearTimeout(timer);
    /* authoritative history is intentionally refreshed once per entry */
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [props.editable]);

  async function perform(label: string, action: () => Promise<unknown>) {
    setBusy(true);
    try {
      await action();
      await Promise.all([reload(), props.refreshLive()]);
      props.onNotice(`${label}已完成`);
    } catch (error) { props.onNotice(error instanceof Error ? error.message : `${label}失败`); }
    finally { setBusy(false); }
  }

  function saveCreate() {
    const start = epoch(draft.plannedStart); const end = epoch(draft.plannedEnd);
    if (start && end && end < start) { props.onNotice("计划结束时间不能早于开始时间"); return; }
    void perform("创建任务", async () => {
      await postJson("/api/v2/data/experiments", { name: draft.name, description: draft.description, planned_start_epoch: start, planned_end_epoch: end, steps: draft.steps.map((title) => ({ title })) });
      setCreating(false); setDraft(emptyDraft());
    });
  }

  function saveEdit() {
    if (!selected) return;
    const start = epoch(draft.plannedStart); const end = epoch(draft.plannedEnd);
    if (start && end && end < start) { props.onNotice("计划结束时间不能早于开始时间"); return; }
    void perform("保存任务", async () => {
      await putJson(`/api/v2/data/experiments/${encodeURIComponent(selected.experiment_id)}`, { experiment_id: selected.experiment_id, name: draft.name, description: draft.description, planned_start_epoch: start, planned_end_epoch: end, steps: draft.steps.map((title) => ({ title })), if_event_seq: selected.last_event_seq });
      setEditing(false);
    });
  }

  function transition(action: "start" | "pause" | "resume" | "complete_step" | "complete" | "cancel") {
    if (!selected) return;
    if ((action === "complete" || action === "cancel") && !window.confirm(action === "complete" ? "确认完成此实验？完成后任务结构将不能再修改。" : "确认取消此实验？此操作会停止关联计时。")) return;
    void perform(action === "start" ? "开始实验" : action === "pause" ? "暂停实验" : action === "resume" ? "继续实验" : action === "complete_step" ? "完成当前步骤" : action === "complete" ? "完成实验" : "取消实验", () => postJson(`/api/v2/data/experiments/${encodeURIComponent(selected.experiment_id)}/transitions`, { experiment_id: selected.experiment_id, action }));
  }

  function addObservation() {
    if (!selected || !observation.trim()) return;
    void perform("记录观察", async () => { await postJson(`/api/v2/data/experiments/${encodeURIComponent(selected.experiment_id)}/observations`, { experiment_id: selected.experiment_id, text: observation.trim() }); setObservation(""); });
  }

  function startTimer() {
    if (!selected) return;
    void perform("启动计时", () => postJson(`/api/v2/data/experiments/${encodeURIComponent(selected.experiment_id)}/timers`, { experiment_id: selected.experiment_id, step_index: selected.current_step, label: timerLabel || "步骤计时", duration_seconds: timerSeconds }));
  }

  function deleteTask() {
    if (!selected) return;
    const confirmation = window.prompt(`输入实验编号 ${selected.experiment_id} 以删除历史记录`);
    if (confirmation !== selected.experiment_id) return;
    // The server remains authoritative; debug mode is obtained from auth state,
    // not inferred from a missing password or an operation-supplied flag.
    const password = privateAdmin ? undefined : window.prompt("请输入管理员密码以确认删除");
    if (!privateAdmin && !password) return;
    void perform("删除实验", () => apiRequest(`/api/v2/data/experiments/${encodeURIComponent(selected.experiment_id)}`, { method: "DELETE", body: JSON.stringify({ confirm_experiment_id: selected.experiment_id, password }) }));
  }

  if (!props.editable) return <section className="single-view"><div className="empty-block"><strong>登录后可管理实验任务</strong><p>管理员可创建任务、设置排期并记录实验全过程。</p></div></section>;

  return <div className="workspace-grid">
    <section className="panel task-list-panel"><header className="panel-header"><h2>实验任务</h2><button onClick={() => { setDraft(emptyDraft()); setCreating(true); setEditing(false); }}>新建任务</button></header><div className="panel-body"><div className="task-toolbar"><span>{loading ? "正在更新…" : `${items.length} 项任务`}</span><button className="secondary" onClick={() => void reload()}>刷新</button></div><div className="experiment-list">{items.map((item) => <button key={item.experiment_id} className={selected?.experiment_id === item.experiment_id ? "selected" : ""} onClick={() => { props.setSelectedId(item.experiment_id); setEditing(false); }}><span className={`state ${item.state.toLowerCase()}`}>{stateName[item.state]}</span><strong>{item.name}</strong><small>{item.planned_start_epoch ? formatTime(item.planned_start_epoch) : item.experiment_id}</small></button>)}{items.length === 0 ? <p className="empty">尚未创建实验任务</p> : null}</div></div></section>
    <section className="panel task-detail-panel"><header className="panel-header"><h2>{creating ? "新建实验任务" : editing ? "编辑实验任务" : selected ? selected.name : "实验详情"}</h2>{selected && !creating && !editing ? <div className="button-row">{selected.state === "READY" ? <button className="secondary" onClick={() => { setDraft(draftOf(selected)); setEditing(true); }}>编辑</button> : null}<button className="danger-outline" onClick={deleteTask}>删除</button></div> : null}</header><div className="panel-body">{selected?.snapshot_pending ? <p role="status">操作已提交，快照待修复；请勿重复执行。</p> : null}{creating ? <TaskForm draft={draft} setDraft={setDraft} submitLabel="创建任务" busy={busy} onSubmit={saveCreate} onCancel={() => setCreating(false)} /> : editing ? <TaskForm draft={draft} setDraft={setDraft} submitLabel="保存任务" busy={busy} onSubmit={saveEdit} onCancel={() => setEditing(false)} /> : selected ? <><div className="task-meta"><span className={`state ${selected.state.toLowerCase()}`}>{stateName[selected.state]}</span><dl><div><dt>计划时间</dt><dd>{formatTime(selected.planned_start_epoch)} — {formatTime(selected.planned_end_epoch)}</dd></div><div><dt>实际时间</dt><dd>{formatTime(selected.started_epoch)} — {formatTime(selected.ended_epoch)}</dd></div><div><dt>最后更新</dt><dd>{formatTime(selected.updated_epoch)}</dd></div></dl></div><p className="observation">{selected.description || "尚未填写实验说明"}</p><ol className="steps">{selected.steps.map((step, index) => <li className={step.completed ? "done" : ""} key={`${step.title}-${index}`}><i>{step.completed ? "✓" : index + 1}</i>{step.title}</li>)}</ol><div className="button-row task-actions">{selected.state === "READY" ? <button disabled={busy} onClick={() => transition("start")}>开始实验</button> : null}{selected.state === "RUNNING" ? <><button className="secondary" disabled={busy} onClick={() => transition("pause")}>暂停</button><button className="secondary" disabled={busy} onClick={() => transition("complete")}>完成</button><button className="danger-outline" disabled={busy} onClick={() => transition("cancel")}>取消</button></> : null}{selected.state === "PAUSED" ? <><button disabled={busy} onClick={() => transition("resume")}>继续实验</button><button className="danger-outline" disabled={busy} onClick={() => transition("cancel")}>取消</button></> : null}</div>{(selected.state === "RUNNING" || selected.state === "PAUSED") ? <div className="task-runtime"><label>观察记录<textarea value={observation} rows={3} maxLength={512} onChange={(event) => setObservation(event.target.value)} /></label><button className="secondary" disabled={busy || !observation.trim()} onClick={addObservation}>追加记录</button>{selected.state === "RUNNING" ? <div className="timer-form"><label>计时名称<input value={timerLabel} maxLength={63} onChange={(event) => setTimerLabel(event.target.value)} /></label><label>秒数<input type="number" min={1} max={604800} value={timerSeconds} onChange={(event) => setTimerSeconds(Math.max(1, Number(event.target.value)))} /></label><button className="secondary" disabled={busy} onClick={startTimer}>启动计时</button></div> : null}</div> : null}<h3>当前观察</h3><p className="observation">{selected.last_observation || "暂无观察记录"}</p></> : <p className="empty">选择任务后查看详情</p>}</div></section>
  </div>;
}

export function TaskCalendar(props: Omit<Props, "onNotice" | "refreshLive"> & { onOpenTask: (id: string) => void }) {
  const [items, setItems] = useState<Experiment[]>(props.fallback);
  const [mode, setMode] = useState<"month" | "week" | "list">("month");
  const [cursor, setCursor] = useState(() => new Date());
  const [state, setState] = useState("ALL");
  const [query, setQuery] = useState("");
  const [loadError, setLoadError] = useState("");
  useEffect(() => {
    if (!props.editable) return;
    const controller = new AbortController();
    void loadExperimentHistory(controller.signal).then((next) => {
      if (!controller.signal.aborted) { setItems(next); setLoadError(""); }
    }).catch((error) => {
      if (!controller.signal.aborted) setLoadError(error instanceof Error ? error.message : "无法读取实验历史");
    });
    return () => controller.abort();
  }, [props.editable, props.fallback]);
  const filtered = useMemo(() => items.filter((item) => (state === "ALL" || item.state === state) && `${item.name} ${item.description}`.toLowerCase().includes(query.toLowerCase())).sort((a, b) => (experimentInterval(a).start ?? Number.MAX_SAFE_INTEGER) - (experimentInterval(b).start ?? Number.MAX_SAFE_INTEGER)), [items, state, query]);
  const dates = useMemo(() => calendarDates(cursor, mode), [cursor, mode]);
  const eventDate = (item: Experiment) => experimentInterval(item).start;
  const move = (delta: number) => setCursor((current) => moveCalendar(current, mode, delta));
return <section className="calendar-view"><header className="single-view-header"><div><p className="section-kicker">Task Calendar</p><h2>计划与历史任务</h2><p>计划任务按排期显示；完成与取消任务优先使用实际时间。</p></div><div className="button-row"><button className="secondary" onClick={() => move(-1)}>上一段</button><strong>{cursor.toLocaleDateString("zh-CN", { year: "numeric", month: "long" })}</strong><button className="secondary" onClick={() => move(1)}>下一段</button></div></header><div className="calendar-controls"><div className="button-row">{(["month", "week", "list"] as const).map((value) => <button key={value} className={mode === value ? "" : "secondary"} onClick={() => setMode(value)}>{value === "month" ? "月" : value === "week" ? "周" : "列表"}</button>)}</div><input placeholder="搜索任务" value={query} onChange={(event) => setQuery(event.target.value)} /><select value={state} onChange={(event) => setState(event.target.value)}><option value="ALL">全部状态</option>{Object.entries(stateName).map(([key, label]) => <option value={key} key={key}>{label}</option>)}</select></div>{loadError ? <p role="alert">历史读取失败：{loadError}。当前显示缓存，不代表完整历史。</p> : null}{mode === "list" ? <div className="calendar-list">{filtered.map((item) => <button key={item.experiment_id} className="calendar-list-item" onClick={() => props.onOpenTask(item.experiment_id)}><span className={`state ${item.state.toLowerCase()}`}>{stateName[item.state]}</span><strong>{item.name}</strong><small>{formatTime(eventDate(item))}</small></button>)}{filtered.length === 0 ? <p className="empty">该条件下没有任务</p> : null}</div> : <div className={`calendar-grid ${mode}`}>{["日", "一", "二", "三", "四", "五", "六"].map((day) => <span className="calendar-weekday" key={day}>{day}</span>)}{dates.map((date) => { const dayItems = filtered.filter((item) => experimentOnDay(item, date)); return <article className={date.getMonth() === cursor.getMonth() || mode === "week" ? "calendar-day" : "calendar-day muted-day"} key={date.toISOString()}><time>{date.getDate()}</time>{dayItems.slice(0, 3).map((item) => <button className={`calendar-event ${item.state.toLowerCase()}`} key={item.experiment_id} onClick={() => props.onOpenTask(item.experiment_id)} title={item.name}>{item.name}</button>)}{dayItems.length > 3 ? <small>+{dayItems.length - 3} 项</small> : null}</article>; })}</div>}</section>;
}
