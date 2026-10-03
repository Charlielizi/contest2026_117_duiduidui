"use client";

import { Component, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import { TelemetryChart } from "../app/TelemetryChart";
import { canRestoreCachedSnapshot, initialSnapshot, navigationFromHash, previewHost, type BoardView } from "./initial-state";
import { clearSnapshotCache, loadLatestSnapshot, saveSnapshot } from "../src/db/labtwin-db";
import { buildReport, canonicalize, downloadText, type ReportBundle } from "../src/report/report";
import {
  findCursorGaps,
  snapshotSchema,
  type DashboardSnapshot,
  type Experiment,
} from "../src/protocol/schema";
import { applyEvent } from "../src/state/dashboard-reducer";
import { DashboardClient, type ConnectionState } from "../src/transport/dashboard-client";
import { apiRequest, ManagementApiError, postJson, putJson, setCsrfToken } from "./api";
import { AgentChat } from "./AgentChat";
import { logoUrl } from "./asset-urls";
import { ExperimentWorkspace, TaskCalendar } from "./ExperimentWorkspace";

type View = BoardView;

type AuthState = {
  initialized: boolean;
  authenticated: boolean;
  role: "guest" | "admin";
  csrf_token?: string;
  private_open?: boolean;
};

type SystemStatus = {
  device_id: string;
  hostname: string;
  lan_hostname: string;
  firmware: string;
  tls_status?: string;
  boot_id: string;
  uptime_seconds: number;
  heap_total_bytes: number;
  heap_used_bytes: number;
  heap_free_bytes: number;
  heap_largest_bytes: number;
  storage_total_bytes: number;
  storage_free_bytes: number;
  storage_level: "normal" | "warning" | "critical" | "blocked";
  wifi: { state: string; ssid: string; ip: string; gateway: string; rssi: number };
  service: { state: string; clients: number };
};

type EnvironmentRule = { rule_id: string; sensor: string; direction: string; trigger: number; clear: number };
type WifiAp = { ssid: string; bssid: string; rssi: number; secure: boolean };
type BleDevice = { name: string; address: string; rssi: number; bonded: boolean };
type BleStatus = { state: string; error: number; devices: BleDevice[] };
type Skill = { name: string; description: string; size: number; mtime: string };
type SkillDetail = { name: string; content: string; revision: string; size: number; mtime: number };
type LogItem = { epoch: number | null; level: string; tag: string; message: string };
type AdvancedSettingsResponse = {
  model: string;
  llm_host: string;
  volc_speaker: string;
  proxy_host: string;
  proxy_port: string;
  volc_appkey: string;
  volc_asr_cluster: string;
  api_key_configured: boolean;
  volc_api_key_configured: boolean;
  volc_token_configured: boolean;
};

const navigation: Array<{ id: View; label: string; admin?: boolean }> = [
  { id: "overview", label: "总览" },
  { id: "experiments", label: "实验任务", admin: true },
  { id: "calendar", label: "任务日历", admin: true },
  { id: "agent", label: "Agent 对话", admin: true },
  { id: "environment", label: "环境监控" },
  { id: "reports", label: "报告中心" },
  { id: "diagnostics", label: "同步诊断" },
  { id: "network", label: "网络与蓝牙", admin: true },
  { id: "device", label: "设备设置", admin: true },
  { id: "ai", label: "AI 与语音", admin: true },
  { id: "skills", label: "技能管理", admin: true },
  { id: "logs", label: "日志诊断", admin: true },
  { id: "maintenance", label: "系统维护", admin: true },
];

const defaultRules: EnvironmentRule[] = [
  { rule_id: "TEMP_HIGH", sensor: "temperature", direction: "high", trigger: 30, clear: 28 },
  { rule_id: "TEMP_LOW", sensor: "temperature", direction: "low", trigger: 10, clear: 12 },
  { rule_id: "HUMIDITY_HIGH", sensor: "humidity", direction: "high", trigger: 70, clear: 65 },
  { rule_id: "HUMIDITY_LOW", sensor: "humidity", direction: "low", trigger: 20, clear: 25 },
];

const stateLabel: Record<Experiment["state"], string> = {
  READY: "待开始",
  RUNNING: "进行中",
  PAUSED: "已暂停",
  COMPLETED: "已完成",
  CANCELLED: "已取消",
  RECOVERY_ERROR: "恢复异常",
};

const eventLabel: Record<string, string> = {
  EXPERIMENT_CREATED: "创建实验",
  EXPERIMENT_STARTED: "开始实验",
  OBSERVATION_ADDED: "新增观察记录",
  TEMP_HIGH_CREATED: "触发高温事件",
  TEMP_HIGH_ACKNOWLEDGED: "确认高温事件",
};

function remaining(dueEpoch: number | null, nowMs: number) {
  if (!dueEpoch) return "--:--";
  const total = Math.max(0, Math.ceil(dueEpoch - nowMs / 1000));
  const hours = Math.floor(total / 3600);
  const minutes = Math.floor((total % 3600) / 60);
  const seconds = total % 60;
  return [hours, minutes, seconds]
    .map((part) => String(part).padStart(2, "0"))
    .join(":");
}

function isDesktopBrowser() {
  return typeof window !== "undefined" &&
    window.matchMedia("(min-width: 980px) and (pointer: fine)").matches;
}

function useTheme() {
  const [theme, setTheme] = useState<"light" | "dark">("light");
  const [themeReady, setThemeReady] = useState(false);

  useEffect(() => {
    const timer = window.setTimeout(() => {
      const initial = document.documentElement.getAttribute("data-theme") === "dark" ? "dark" : "light";
      setTheme(initial);
      setThemeReady(true);
    }, 0);
    return () => window.clearTimeout(timer);
  }, []);

  useEffect(() => {
    if (themeReady) document.documentElement.setAttribute("data-theme", theme);
  }, [theme, themeReady]);

  function toggle() {
    setTheme((current) => {
      const next = current === "light" ? "dark" : "light";
      try { localStorage.setItem("labtwin-theme", next); } catch { /* storage may be disabled */ }
      return next;
    });
  }

  return { theme, toggle };
}

function formatBytes(value = 0) {
  if (value >= 1024 ** 3) return `${(value / 1024 ** 3).toFixed(1)} GiB`;
  if (value >= 1024 ** 2) return `${(value / 1024 ** 2).toFixed(1)} MiB`;
  if (value >= 1024) return `${(value / 1024).toFixed(1)} KiB`;
  return `${value} B`;
}

function formatTime(epoch: number | null | undefined) {
  return epoch ? new Date(epoch * 1000).toLocaleString("zh-CN", { hour12: false }) : "板端时间不可信";
}

function Panel(props: { title: string; action?: React.ReactNode; children: React.ReactNode; className?: string }) {
  return (
    <section className={`panel ${props.className ?? ""}`}>
      <header className="panel-header"><h2>{props.title}</h2>{props.action}</header>
      <div className="panel-body">{props.children}</div>
    </section>
  );
}

function NavIcon({ view }: { view: View }) {
  const common = {
    viewBox: "0 0 24 24",
    fill: "none",
    stroke: "currentColor",
    strokeWidth: 1.8,
    strokeLinecap: "round" as const,
    strokeLinejoin: "round" as const,
  };
  switch (view) {
    case "overview":
      return <svg {...common}><rect x="3" y="3" width="7" height="7" rx="1.5" /><rect x="14" y="3" width="7" height="7" rx="1.5" /><rect x="3" y="14" width="7" height="7" rx="1.5" /><rect x="14" y="14" width="7" height="7" rx="1.5" /></svg>;
    case "experiments":
      return <svg {...common}><path d="M9 3h6M10 3v6l-5 9a2 2 0 0 0 2 3h10a2 2 0 0 0 2-3l-5-9V3" /><path d="M7.5 14h9" /></svg>;
    case "calendar":
      return <svg {...common}><rect x="3" y="5" width="18" height="16" rx="2" /><path d="M16 3v4M8 3v4M3 10h18" /></svg>;
    case "agent":
      return <svg {...common}><path d="M7 18.5 3 21l1.5-4A8 8 0 1 1 20 12a8 8 0 0 1-13 6.5Z" /><path d="M8 12h.01M12 12h.01M16 12h.01" /></svg>;
    case "environment":
      return <svg {...common}><path d="M14 14.76V3.5a2.5 2.5 0 0 0-5 0v11.26a4.5 4.5 0 1 0 5 0z" /></svg>;
    case "reports":
      return <svg {...common}><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z" /><path d="M14 3v5h5" /><line x1="9" y1="13" x2="15" y2="13" /><line x1="9" y1="17" x2="13" y2="17" /></svg>;
    case "diagnostics":
      return <svg {...common}><path d="M3 12h4l3 8 4-16 3 8h4" /></svg>;
    case "network":
      return <svg {...common}><path d="M5 12.55a11 11 0 0 1 14 0M8.5 16.05a6 6 0 0 1 7 0M12 20h.01" /></svg>;
    case "device":
      return <svg {...common}><circle cx="12" cy="12" r="3" /><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z" /></svg>;
    case "ai":
      return <svg {...common}><path d="M12 1a3 3 0 0 0-3 3v8a3 3 0 0 0 6 0V4a3 3 0 0 0-3-3z" /><path d="M19 10v2a7 7 0 0 1-14 0v-2M12 19v4" /></svg>;
    case "skills":
      return <svg {...common}><polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2" /></svg>;
    case "logs":
      return <svg {...common}><rect x="3" y="4" width="18" height="16" rx="2" /><path d="M7 9l3 3-3 3M13 15h4" /></svg>;
    case "maintenance":
      return <svg {...common}><path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.77-3.77a6 6 0 0 1-7.94 7.94l-6.91 6.91a2.12 2.12 0 0 1-3-3l6.91-6.91a6 6 0 0 1 7.94-7.94l-3.76 3.76z" /></svg>;
    default:
      return <svg {...common}><circle cx="12" cy="12" r="9" /></svg>;
  }
}

class PortalErrorBoundary extends Component<{ children: ReactNode }, { failed: boolean }> {
  state = { failed: false };

  static getDerivedStateFromError() {
    return { failed: true };
  }

  componentDidCatch(error: Error) {
    console.error("LabTwin Portal render failure", error);
  }

  render() {
    if (this.state.failed) {
      return <main className="app-shell"><section className="content-stack"><article className="panel"><p className="section-kicker">Portal Recovery</p><h1>管理页面未能完整加载</h1><p>请刷新页面；若问题持续，请清除本网站缓存后重试。</p><button className="primary-button" onClick={() => window.location.reload()}>刷新页面</button></article></section></main>;
    }
    return this.props.children;
  }
}

export function BoardAdminApp() {
  return <PortalErrorBoundary><BoardAdminContent /></PortalErrorBoundary>;
}

function BoardAdminContent() {
  const [view, setView] = useState<View>(() => navigationFromHash(typeof window === "undefined" ? "" : window.location.hash).view);
  const [snapshot, setSnapshot] = useState<DashboardSnapshot>(() => initialSnapshot(typeof location !== "undefined" && previewHost(location.hostname)));
  const liveSnapshotSeen = useRef(false);
  const [connection, setConnection] = useState<ConnectionState>("connecting");
  const [detail, setDetail] = useState("正在连接板卡");
  const [auth, setAuth] = useState<AuthState>({ initialized: false, authenticated: false, role: "guest" });
  const [status, setStatus] = useState<SystemStatus | null>(null);
  const [loginOpen, setLoginOpen] = useState(false);
  const [notice, setNotice] = useState("");
  const [busy, setBusy] = useState(false);
  const [rules, setRules] = useState(defaultRules);
  const [wifiAps, setWifiAps] = useState<WifiAp[]>([]);
  const [ble, setBle] = useState<BleStatus>({ state: "off", error: 0, devices: [] });
  const [skills, setSkills] = useState<Skill[]>([]);
  const [logs, setLogs] = useState<LogItem[]>([]);
  const [selectedId, setSelectedId] = useState(() => navigationFromHash(typeof window === "undefined" ? "" : window.location.hash).selectedId);
  const [report, setReport] = useState<ReportBundle | null>(null);
  const [nowMs, setNowMs] = useState(0);
  const [desktopEnhanced, setDesktopEnhanced] = useState(false);
  const { theme, toggle: toggleTheme } = useTheme();

  const selected = snapshot.experiments.find((item) => item.experiment_id === selectedId) ?? snapshot.experiments[0];
  const activeAlerts = snapshot.environment_events.filter((item) => item.state !== "RECOVERED");
  const latest = snapshot.telemetry[snapshot.telemetry.length - 1];
  const storagePercent = status?.storage_total_bytes
    ? Math.round(((status.storage_total_bytes - status.storage_free_bytes) / status.storage_total_bytes) * 100)
    : 0;

  useEffect(() => {
    const enhanced = isDesktopBrowser();
    const enhancementTimer = window.setTimeout(() => setDesktopEnhanced(enhanced), 0);
    const preview = location.hostname === "localhost" || location.hostname === "127.0.0.1";
    if (!preview) void refreshPublic();
    const authTimer = preview ? null : window.setInterval(() => void refreshPublic(), 5_000);
    const clockTimer = window.setInterval(() => setNowMs(Date.now()), 1_000);
    return () => {
      window.clearTimeout(enhancementTimer);
      if (authTimer !== null) window.clearInterval(authTimer);
      window.clearInterval(clockTimer);
    };
  }, []);

  useEffect(() => {
    const preview = location.hostname === "localhost" || location.hostname === "127.0.0.1";
    if (!preview && !auth.authenticated) return;

    let cancelled = false;
    void (async () => {
      if (desktopEnhanced && !preview && status) {
        const cached = await loadLatestSnapshot();
        if (!cancelled && cached && canRestoreCachedSnapshot(cached, status, liveSnapshotSeen.current)) setSnapshot(cached);
      }
    })();
    const scheme = location.protocol === "https:" ? "wss" : "ws";
    const client = new DashboardClient({
      endpoint: preview ? "demo://labtwin" : `${scheme}://${location.host}/labtwin`,
      cursor: snapshot.cursor,
      onState: (state, message) => { setConnection(state); setDetail(message ?? ""); },
      onSnapshot: (incoming) => { liveSnapshotSeen.current = true; setSnapshot(incoming); },
      onEvent: (event) => setSnapshot((current) => applyEvent(current, event)),
    });
    client.connect();
    return () => { cancelled = true; client.disconnect(); };
    // The cursor is intentionally captured once; the server sync response replaces it.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [auth.authenticated, desktopEnhanced, status?.device_id, status?.boot_id]);

  useEffect(() => {
    if (!desktopEnhanced || !auth.authenticated || !liveSnapshotSeen.current) return;
    const timer = window.setTimeout(() => { void saveSnapshot(snapshot); }, 500);
    return () => window.clearTimeout(timer);
  }, [auth.authenticated, desktopEnhanced, snapshot]);

  useEffect(() => {
    const params = new URLSearchParams();
    params.set("view", view);
    if (selectedId) params.set("id", selectedId);
    const next = `#${params.toString()}`;
    if (window.location.hash !== next) {
      window.history.replaceState(null, "", next);
    }
  }, [view, selectedId]);

  useEffect(() => {
    if (!auth.authenticated) return;
    if (view === "skills") void loadSkills();
    if (view === "logs") void loadLogs();
    if (view === "network") { void loadWifiStatus(); void loadBluetooth(); }
    if (view === "environment") void loadRules();
  }, [auth.authenticated, view]);

  async function refreshPublic() {
    try {
      const authResult = await apiRequest<AuthState>("/api/v2/auth/state");
      if (!authResult.data) return;
      setAuth(authResult.data);
      if (authResult.data.csrf_token) setCsrfToken(authResult.data.csrf_token);
      if (!authResult.data.authenticated) {
        setCsrfToken("");
        setStatus(null);
        liveSnapshotSeen.current = false;
        setSnapshot(initialSnapshot(false));
        setSelectedId("");
        setConnection("offline");
        setDetail(authResult.data.initialized ? "请以管理员身份登录" : "请设置管理员密码");
        return;
      }
      const [statusResult, snapshotResult] = await Promise.all([
        apiRequest<SystemStatus>("/api/v2/status"),
        apiRequest<DashboardSnapshot>("/api/v2/labtwin/snapshot"),
      ]);
      if (statusResult.data) setStatus(statusResult.data);
      if (snapshotResult.data) {
        const parsed = snapshotSchema.safeParse(snapshotResult.data);
        if (parsed.success) {
          liveSnapshotSeen.current = true;
          setSnapshot(parsed.data);
        } else {
          setDetail("板端快照格式异常，已保留安全视图");
        }
      }
    } catch {
      setDetail("无法读取管理状态");
    }
  }

  async function logout() {
    await postJson("/api/v2/auth/logout", {});
    setCsrfToken("");
    setAuth({ initialized: true, authenticated: false, role: "guest" });
    setStatus(null);
    liveSnapshotSeen.current = false;
    setSnapshot(initialSnapshot(false));
    setSelectedId("");
    setView("overview");
    await clearSnapshotCache();
  }

  async function run(label: string, action: () => Promise<unknown>) {
    setBusy(true); setNotice("");
    try { await action(); setNotice(`${label}已完成`); }
    catch (error) { setNotice(error instanceof Error ? error.message : `${label}失败`); }
    finally { setBusy(false); }
  }

  async function loadRules() {
    try {
      const result = await apiRequest<{ rules: EnvironmentRule[] }>("/api/v2/settings/environment");
      if (result.data?.rules) setRules(result.data.rules);
    } catch { /* keep defaults */ }
  }

  async function loadWifiStatus() {
    try {
      const result = await apiRequest<{ aps: WifiAp[] }>("/api/v2/network/status");
      if (result.data?.aps) setWifiAps(result.data.aps);
    } catch { /* status card remains available */ }
  }

  async function loadBluetooth() {
    try {
      const result = await apiRequest<BleStatus>("/api/v2/bluetooth/status");
      if (result.data) setBle(result.data);
    } catch { /* Bluetooth may be unavailable on compatibility firmware. */ }
  }

  async function loadSkills() {
    const result = await apiRequest<{ skills: Skill[] }>("/api/v2/skills");
    setSkills(result.data?.skills ?? []);
  }

  async function loadLogs() {
    const result = await apiRequest<{ logs: LogItem[] }>("/api/v2/logs?limit=200");
    setLogs(result.data?.logs ?? []);
  }

  function openAdmin(nextView?: View) {
    if (auth.authenticated) { if (nextView) setView(nextView); return; }
    setLoginOpen(true);
  }

  async function createReport() {
    if (!selected) return;
    const bundle = await buildReport(snapshot, selected.experiment_id);
    setReport(bundle); setView("reports");
  }

  const pageTitle = navigation.find((item) => item.id === view)?.label ?? "总览";

  return (
    <div className="app-shell unified-portal">
      <aside className="sidebar portal-sidebar" aria-label="LabTwin 主导航">
        <div className="brand-mark" aria-label="LabTwin">
          {/* Shared by the Next preview and the Vite board build. */}
          {/* eslint-disable-next-line @next/next/no-img-element */}
          <img src={logoUrl} alt="" aria-hidden="true" />
        </div>
        <nav aria-label="主导航">
          {navigation.map((item) => (
            <button
              key={item.id}
              className={view === item.id ? "nav-button active" : "nav-button"}
              onClick={() => item.admin ? openAdmin(item.id) : setView(item.id)}
              title={item.label}
            >
              <span className="nav-icon"><NavIcon view={item.id} /></span>
              <small>{item.label}</small>
              {item.admin && !auth.authenticated ? <i className="nav-lock">锁</i> : null}
            </button>
          ))}
        </nav>
        <button className="theme-toggle" onClick={toggleTheme} aria-label={theme === "light" ? "切换到深色模式" : "切换到浅色模式"} title={theme === "light" ? "切换到深色模式" : "切换到浅色模式"}>
          {theme === "light" ? (
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z" /></svg>
          ) : (
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><circle cx="12" cy="12" r="5" /><line x1="12" y1="1" x2="12" y2="3" /><line x1="12" y1="21" x2="12" y2="23" /><line x1="4.22" y1="4.22" x2="5.64" y2="5.64" /><line x1="18.36" y1="18.36" x2="19.78" y2="19.78" /><line x1="1" y1="12" x2="3" y2="12" /><line x1="21" y1="12" x2="23" y2="12" /><line x1="4.22" y1="19.78" x2="5.64" y2="18.36" /><line x1="18.36" y1="5.64" x2="19.78" y2="4.22" /></svg>
          )}
        </button>
        <div className="sidebar-foot">
          <span className={`status-dot ${connection}`} />
          <div><strong>{connection === "online" ? "板卡在线" : connection === "demo" ? "演示模式" : "连接中"}</strong><small>{detail}</small></div>
        </div>
      </aside>

      <main className="main-area">
        <header className="topbar">
          <div><p className="eyebrow">Gemini-S1 · Unified Lab Portal</p><h1>{pageTitle}</h1></div>
          <div className="topbar-actions">
            {desktopEnhanced ? <span className="desktop-enhancement">电脑增强已启用</span> : null}
            <div className={`connection-badge ${connection}`}><i aria-hidden="true" /><span>{connection === "online" ? "板卡在线" : connection === "demo" ? "演示数据" : connection}</span></div>
            <span className={`storage-pill ${status?.storage_level ?? "normal"}`}>存储 {storagePercent}%</span>
            <button className="secondary-button" onClick={() => auth.authenticated ? setView("device") : openAdmin()}>
              {auth.authenticated ? "管理员" : "管理员登录"}
            </button>
          </div>
        </header>

        <div className="device-strip">
          <div><span className="device-name">{status?.device_id ?? snapshot.device.device_id}</span><span>{status?.firmware ?? snapshot.device.firmware}</span></div>
          <div><span>{status?.hostname ?? "labtwin.local"}</span><span>{status?.wifi.ip || detail}</span></div>
        </div>

        {notice ? <div className="notice" role="status">{notice}<button onClick={() => setNotice("")}>关闭</button></div> : null}
        {status?.tls_status === "TLS_TIME_UNSYNCED" ? <div className="notice" role="status">板端时间尚未同步，云端对话与语音连接已暂时阻止。</div> : status?.tls_status === "TLS_TRUST_UNAVAILABLE" ? <div className="notice" role="alert">证书信任根不可用，云端连接已阻止，请检查固件资源。</div> : null}

        <div className="content">
          {view === "overview" && <Overview snapshot={snapshot} status={status} latest={latest} activeAlerts={activeAlerts.length} selectedId={selectedId} setSelectedId={setSelectedId} nowMs={nowMs} onReport={createReport} />}
          {view === "experiments" && <div className="admin-surface"><ExperimentWorkspace privateAdmin={auth.private_open === true} fallback={snapshot.experiments} selectedId={selectedId} setSelectedId={setSelectedId} editable={auth.authenticated} refreshLive={refreshPublic} onNotice={setNotice} /></div>}
          {view === "calendar" && <div className="admin-surface"><TaskCalendar fallback={snapshot.experiments} selectedId={selectedId} setSelectedId={setSelectedId} editable={auth.authenticated} onOpenTask={(id) => { setSelectedId(id); setView("experiments"); }} /></div>}
          {view === "agent" && <div className="admin-surface"><AgentChat authenticated={auth.authenticated} onNotice={setNotice} onExperimentChanged={refreshPublic} /></div>}
          {view === "environment" && <Environment snapshot={snapshot} rules={rules} setRules={setRules} editable={auth.authenticated} busy={busy} run={run} />}
          {view === "reports" && <Reports snapshot={snapshot} selectedId={selectedId} setSelectedId={setSelectedId} report={report} createReport={createReport} desktopEnhanced={desktopEnhanced} />}
          {view === "diagnostics" && <Diagnostics snapshot={snapshot} status={status} connection={connection} detail={detail} desktopEnhanced={desktopEnhanced} />}
          {view === "network" && <div className="admin-surface"><Network status={status} aps={wifiAps} busy={busy} run={run} reload={loadWifiStatus} /><Bluetooth status={ble} busy={busy} run={run} reload={loadBluetooth} /></div>}
          {view === "device" && <div className="admin-surface"><Device status={status} auth={auth} busy={busy} run={run} logout={logout} /></div>}
          {view === "ai" && <div className="admin-surface"><AdvancedSettings busy={busy} run={run} /></div>}
          {view === "skills" && <div className="admin-surface"><Skills skills={skills} busy={busy} run={run} reload={loadSkills} /></div>}
          {view === "logs" && <div className="admin-surface"><Logs logs={logs} reload={loadLogs} /></div>}
          {view === "maintenance" && <div className="admin-surface"><Maintenance busy={busy} run={run} status={status} /></div>}
        </div>
      </main>
      {loginOpen ? <LoginDialog auth={auth} close={() => setLoginOpen(false)} success={(next) => { setAuth(next); if (next.csrf_token) setCsrfToken(next.csrf_token); setLoginOpen(false); void refreshPublic(); }} /> : null}
    </div>
  );
}

function TimeLinePanel({ events }: { events: DashboardSnapshot["events"] }) {
  const [filter, setFilter] = useState<"all" | "experiment" | "environment" | "telemetry">("all");
  const filtered = filter === "all" ? events : events.filter((item) => item.stream === filter);
  const sorted = [...filtered].sort((a, b) => (b.timestamp_epoch ?? 0) - (a.timestamp_epoch ?? 0) || b.seq - a.seq);
  const filters: Array<{ id: typeof filter; label: string }> = [
    { id: "all", label: "全部" },
    { id: "experiment", label: "实验" },
    { id: "environment", label: "环境" },
    { id: "telemetry", label: "遥测" },
  ];
  return <article className="panel timeline-panel">
    <div className="panel-heading"><div><p className="section-kicker">Audit Trail</p><h2>事实时间线</h2></div>
      <div className="timeline-filter">{filters.map((f) => <button key={f.id} className={filter === f.id ? "active" : ""} onClick={() => setFilter(f.id)}>{f.label}</button>)}</div>
    </div>
    <div className="timeline">
      {sorted.map((event) => <div className="timeline-item" key={`${event.stream}-${event.subject_id}-${event.seq}`}><i className={`event-dot ${event.stream}`} /><div><strong>{eventLabel[event.event_type] ?? event.event_type}</strong><p>{event.subject_id} · {event.source} · seq {event.seq}</p></div><time>{formatTime(event.timestamp_epoch)}</time></div>)}
      {sorted.length === 0 ? <p className="empty">无匹配事件</p> : null}
    </div>
  </article>;
}

function Overview({ snapshot, status, latest, activeAlerts, selectedId, setSelectedId, nowMs, onReport }: {
  snapshot: DashboardSnapshot;
  status: SystemStatus | null;
  latest: DashboardSnapshot["telemetry"][number] | undefined;
  activeAlerts: number;
  selectedId: string;
  setSelectedId: (id: string) => void;
  nowMs: number;
  onReport: () => void;
}) {
  const running = snapshot.experiments.filter((item) => item.state === "RUNNING");
  const selected = snapshot.experiments.find((item) => item.experiment_id === selectedId) ?? snapshot.experiments[0];
  const gaps = findCursorGaps(snapshot.events);
  const alerts = snapshot.environment_events.filter((item) => item.state !== "RECOVERED");

  return <div className="content-stack">
    <section className="metric-grid" aria-label="关键指标">
      <article className="metric-card accent-teal"><p>运行中实验</p><strong>{running.length}</strong><span>{snapshot.experiments.length} 个实验已载入</span></article>
      <article className="metric-card accent-amber"><p>活动环境事件</p><strong>{activeAlerts}</strong><span>{alerts[0]?.rule_id ?? "当前无告警"}</span></article>
      <article className="metric-card accent-blue"><p>当前温度</p><strong>{latest?.temperature_avg?.toFixed(2) ?? "--"}<em>°C</em></strong><span>{snapshot.device.sensor_stale ? "数据陈旧" : "采样正常"}</span></article>
      <article className="metric-card accent-slate"><p>证据链状态</p><strong className="word-value">{gaps.length ? "不完整" : "连续"}</strong><span>{snapshot.events.length} 条事实事件</span></article>
    </section>

    <section className="dashboard-grid">
      <article className="panel experiments-panel">
        <div className="panel-heading"><div><p className="section-kicker">Experiments</p><h2>实验队列</h2></div><span className="read-only-tag">访客只读</span></div>
        <div className="experiment-table" role="table" aria-label="实验列表">
          <div className="table-row table-header" role="row"><span>实验</span><span>状态</span><span>步骤</span><span>计时</span></div>
          {snapshot.experiments.map((experiment) => {
            const timer = experiment.timers.find((item) => item.state === "RUNNING");
            return <button key={experiment.experiment_id} className={selected?.experiment_id === experiment.experiment_id ? "table-row selected" : "table-row"} onClick={() => setSelectedId(experiment.experiment_id)} role="row">
              <span className="experiment-name"><b>{experiment.experiment_id}</b><small>{experiment.name}</small></span>
              <span><i className={`state-pill state-${experiment.state.toLowerCase()}`}>{stateLabel[experiment.state]}</i></span>
              <span>{Math.min(experiment.current_step + 1, experiment.steps.length)} / {experiment.steps.length}</span>
              <span className="mono timer-value">{timer ? remaining(timer.due_epoch, nowMs) : "—"}</span>
            </button>;
          })}
        </div>
        {selected ? <div className="selected-summary"><div><p>当前步骤</p><strong>{selected.steps[selected.current_step]?.title ?? "已结束"}</strong></div><div><p>最近观察</p><strong>{selected.last_observation || "暂无观察记录"}</strong></div><button className="primary-button" onClick={onReport}>生成实验报告</button></div> : null}
      </article>

      <article className="panel environment-panel">
        <div className="panel-heading"><div><p className="section-kicker">Environment</p><h2>环境状态</h2></div><span className={snapshot.device.sensor_stale ? "health bad" : "health"}>{snapshot.device.sensor_stale ? "数据陈旧" : "采样正常"}</span></div>
        <div className="environment-values"><div><span>温度</span><strong>{latest?.temperature_avg?.toFixed(2) ?? "--"}<em>°C</em></strong></div><div><span>湿度</span><strong>{latest?.humidity_avg?.toFixed(2) ?? "--"}<em>%RH</em></strong></div></div>
        {alerts.slice(0, 3).map((alert) => <div className="alert-card" key={alert.event_id}><div className="alert-code">!</div><div><strong>{alert.rule_id}</strong><p>{alert.measured_value.toFixed(2)} / 触发阈值 {alert.trigger_threshold}</p><span>{alert.event_id} · {alert.state}</span></div></div>)}
        <div className="rule-list"><div><span>网络地址</span><b>{status?.wifi.ip || "--"}</b></div><div><span>Wi‑Fi</span><b>{status?.wifi.ssid || "等待连接"}</b></div></div>
      </article>

      <article className="panel telemetry-panel"><div className="panel-heading"><div><p className="section-kicker">Telemetry · 5 Minute / Hourly Archive</p><h2>温湿度趋势</h2></div><span className="subtle-text">近 7 天每 5 分钟，较早数据每小时</span></div><TelemetryChart points={snapshot.telemetry} /></article>

      <TimeLinePanel events={snapshot.events} />
    </section>
  </div>;
}

function Environment({ snapshot, rules, setRules, editable, busy, run }: { snapshot: DashboardSnapshot; rules: EnvironmentRule[]; setRules: (rules: EnvironmentRule[]) => void; editable: boolean; busy: boolean; run: (label: string, action: () => Promise<unknown>) => Promise<void> }) {
  const update = (index: number, field: "trigger" | "clear", value: number) => setRules(rules.map((rule, i) => i === index ? { ...rule, [field]: value } : rule));
  const thresholds = {
    tempHigh: rules.find((r) => r.rule_id === "TEMP_HIGH")?.trigger,
    tempLow: rules.find((r) => r.rule_id === "TEMP_LOW")?.trigger,
    humidityHigh: rules.find((r) => r.rule_id === "HUMIDITY_HIGH")?.trigger,
    humidityLow: rules.find((r) => r.rule_id === "HUMIDITY_LOW")?.trigger,
  };
  return <div className="content-stack"><Panel title="环境趋势"><TelemetryChart points={snapshot.telemetry} thresholds={thresholds} /></Panel><Panel title="阈值规则" action={editable ? <div className="button-row"><button className="secondary" disabled={busy} onClick={() => void run("恢复默认阈值", async () => { await postJson("/api/v2/settings/environment/reset", {}); setRules(defaultRules); })}>恢复默认</button><button disabled={busy} onClick={() => void run("保存阈值", () => putJson("/api/v2/settings/environment", { rules }))}>保存</button></div> : <span className="muted">管理员可编辑</span>}><div className="rules-grid">{rules.map((rule, index) => <label key={rule.rule_id}><strong>{rule.rule_id}</strong><span>{rule.sensor} / {rule.direction}</span><div><input type="number" step="0.1" disabled={!editable} value={rule.trigger} onChange={(event) => update(index, "trigger", Number(event.target.value))} /><b>触发</b></div><div><input type="number" step="0.1" disabled={!editable} value={rule.clear} onChange={(event) => update(index, "clear", Number(event.target.value))} /><b>清除</b></div></label>)}</div></Panel><Panel title="告警记录"><table><thead><tr><th>规则</th><th>测量值</th><th>状态</th><th>实验</th><th>发生时间</th></tr></thead><tbody>{snapshot.environment_events.map((event) => <tr key={event.event_id}><td>{event.rule_id}</td><td>{event.measured_value}</td><td>{event.state}</td><td>{event.experiment_id ?? "-"}</td><td>{formatTime(event.created_epoch)}</td></tr>)}</tbody></table></Panel></div>;
}

function Reports({ snapshot, selectedId, setSelectedId, report, createReport, desktopEnhanced }: { snapshot: DashboardSnapshot; selectedId: string; setSelectedId: (id: string) => void; report: ReportBundle | null; createReport: () => Promise<void>; desktopEnhanced: boolean }) {
  async function exportDocx() { if (report) (await import("./report-export")).downloadDocx(report, selectedId); }
  async function exportZip() { if (report) (await import("./report-export")).downloadEvidenceZip(report, selectedId); }
  function exportMarkdown() { if (report) downloadText(`LabTwin-${selectedId}-report.md`, report.markdown, "text/markdown;charset=utf-8"); }
  function exportEvidence() { if (report) downloadText(`LabTwin-${selectedId}-evidence.json`, canonicalize(report.evidence), "application/json;charset=utf-8"); }
  return <section className="single-view report-view">
    <div className="single-view-header"><div><p className="section-kicker">Traceable Report</p><h2>{selectedId || "—"} 可追溯实验报告</h2><p>确定性模板只使用板端事实事件，AI 不参与事实生成。</p></div><div className="button-row"><select value={selectedId} onChange={(event) => setSelectedId(event.target.value)}>{snapshot.experiments.map((item) => <option key={item.experiment_id} value={item.experiment_id}>{item.experiment_id} · {item.name}</option>)}</select><button className="secondary-button" onClick={() => void createReport()}>生成/刷新</button><button className="secondary-button" disabled={!report} onClick={exportEvidence}>Evidence</button><button className="primary-button" disabled={!report} onClick={exportMarkdown}>Markdown</button><button className="text-button" disabled={!report} onClick={() => window.print()}>打印 PDF</button></div></div>
    {!report ? <div className="empty-report"><span>REPORT</span><h3>尚未生成报告</h3><p>选择实验后生成确定性报告和证据清单。</p><button className="primary-button" onClick={() => void createReport()}>生成报告</button></div> : <div className="report-layout"><article className="report-paper"><pre>{report.markdown}</pre></article><aside className="evidence-panel"><p className="section-kicker">Evidence Health</p><h3>{report.evidence.integrity.complete ? "证据完整" : "存在保留项"}</h3><dl><div><dt>引用事件</dt><dd>{report.evidence.events.length}</dd></div><div><dt>序号空洞</dt><dd>{report.evidence.integrity.cursor_gaps.length}</dd></div><div><dt>板端时钟</dt><dd>{report.evidence.integrity.clock_trusted ? "可信" : "不可信"}</dd></div><div><dt>存储状态</dt><dd>{report.evidence.integrity.storage_error ? "异常" : "正常"}</dd></div></dl><p className="hash-label">SHA-256</p><code>{report.digest}</code>{desktopEnhanced ? <div className="form-stack"><span className="desktop-enhancement">电脑浏览器增强导出</span><button onClick={() => void exportDocx()}>下载 Word</button><button onClick={() => void exportZip()}>下载证据 ZIP</button></div> : null}</aside></div>}
  </section>;
}

function Diagnostics({ snapshot, status, connection, detail, desktopEnhanced }: { snapshot: DashboardSnapshot; status: SystemStatus | null; connection: ConnectionState; detail: string; desktopEnhanced: boolean }) {
  const gaps = findCursorGaps(snapshot.events);
  return <section className="single-view diagnostics-view"><div className="single-view-header"><div><p className="section-kicker">Read-only Diagnostics</p><h2>同步与数据质量</h2><p>访客可查看脱敏诊断；管理员凭据、Wi-Fi 密码和 API 密钥不会显示。</p></div>{desktopEnhanced ? <span className="desktop-enhancement">IndexedDB 本地缓存已启用</span> : null}</div><div className="diagnostic-grid"><article><span>协议</span><strong>{snapshot.device.protocol}</strong><small>schema v1</small></article><article><span>设备 ID</span><strong>{status?.device_id ?? snapshot.device.device_id}</strong><small>{status?.boot_id ?? snapshot.device.boot_id}</small></article><article><span>连接状态</span><strong>{connection}</strong><small>{detail}</small></article><article><span>板端时钟</span><strong>{snapshot.device.clock_trusted ? "可信" : "不可信"}</strong><small>{formatTime(snapshot.device.sampled_epoch)}</small></article><article><span>实验游标</span><strong>{Object.keys(snapshot.cursor.experiments).length} streams</strong><small>{JSON.stringify(snapshot.cursor.experiments)}</small></article><article><span>环境游标</span><strong>{snapshot.cursor.environment}</strong><small>telemetry {snapshot.cursor.telemetry}</small></article><article><span>序号空洞</span><strong>{gaps.length}</strong><small>{gaps.join("、") || "未发现"}</small></article><article><span>板端存储</span><strong>{snapshot.device.storage_error ? "异常" : "正常"}</strong><small>{status?.storage_level ?? "等待状态"}</small></article></div></section>;
}

function Network({ status, aps, busy, run, reload }: { status: SystemStatus | null; aps: WifiAp[]; busy: boolean; run: (label: string, action: () => Promise<unknown>) => Promise<void>; reload: () => Promise<void> }) {
  const [ssid, setSsid] = useState(""); const [password, setPassword] = useState("");
  return <div className="two-column"><Panel title="当前连接"><dl className="detail-list"><div><dt>状态</dt><dd>{status?.wifi.state ?? "未知"}</dd></div><div><dt>SSID</dt><dd>{status?.wifi.ssid || "未连接"}</dd></div><div><dt>IP</dt><dd>{status?.wifi.ip || "-"}</dd></div><div><dt>网关</dt><dd>{status?.wifi.gateway || "-"}</dd></div><div><dt>信号</dt><dd>{status?.wifi.rssi ?? 0} dBm</dd></div></dl><div className="button-row"><button className="secondary" disabled={busy} onClick={() => void run("断开网络", () => postJson("/api/v2/network/disconnect", {}))}>断开</button><button className="danger-outline" disabled={busy} onClick={() => void run("忘记网络", () => postJson("/api/v2/network/forget", {}))}>忘记</button></div></Panel><Panel title="连接新网络" action={<button className="secondary" disabled={busy} onClick={() => void run("扫描网络", async () => { await postJson("/api/v2/network/scan", {}); await new Promise((resolve) => setTimeout(resolve, 1800)); await reload(); })}>重新扫描</button>}><form className="form-stack" onSubmit={(event) => { event.preventDefault(); void run("Wi‑Fi 切换", () => postJson("/api/v2/network/connect", { ssid, password })); }}><label>网络名称<input value={ssid} maxLength={32} required onChange={(event) => setSsid(event.target.value)} /></label><label>密码<input type="password" value={password} maxLength={63} onChange={(event) => setPassword(event.target.value)} /></label><p className="warning-copy">切换后页面会暂时断开；失败将在 90 秒内自动恢复旧网络。</p><button disabled={busy}>保存并切换</button></form><div className="ap-list">{aps.map((ap) => <button key={`${ap.ssid}-${ap.bssid}`} onClick={() => setSsid(ap.ssid)}><strong>{ap.ssid}</strong><span>{ap.secure ? "加密" : "开放"} · {ap.rssi} dBm</span></button>)}</div></Panel></div>;
}

function Bluetooth({ status, busy, run, reload }: { status: BleStatus; busy: boolean; run: (label: string, action: () => Promise<unknown>) => Promise<void>; reload: () => Promise<void> }) {
  const enabled = status.state !== "off";
  const action = async (path: string, body: unknown, label: string) => {
    await run(label, async () => {
      await postJson(`/api/v2/bluetooth/${path}`, body);
      await new Promise((resolve) => setTimeout(resolve, path === "scan" ? 1200 : 400));
      await reload();
    });
  };
  return <Panel title="蓝牙设备" action={<div className="button-row"><button className="secondary" disabled={busy} onClick={() => void action("enable", { enabled: !enabled }, enabled ? "关闭蓝牙" : "开启蓝牙")}>{enabled ? "关闭蓝牙" : "开启蓝牙"}</button><button disabled={busy || !enabled} onClick={() => void action("scan", {}, "扫描蓝牙")}>扫描</button></div>}><p className="section-copy">状态：{status.state}{status.error ? ` · 错误 ${status.error}` : ""}</p><div className="skill-list">{status.devices.map((device) => <article key={device.address}><div><strong>{device.name || "未命名设备"}</strong><small>{device.address} · {device.rssi} dBm · {device.bonded ? "已配对" : "未配对"}</small></div><button className={device.bonded ? "danger-outline" : "secondary"} disabled={busy} onClick={() => void action(device.bonded ? "unpair" : "pair", { address: device.address }, device.bonded ? "取消蓝牙配对" : "蓝牙配对")}>{device.bonded ? "取消配对" : "配对"}</button></article>)}{enabled && status.devices.length === 0 ? <p className="empty">尚未发现蓝牙设备</p> : null}</div></Panel>;
}

function Device({ status, auth, busy, run, logout }: { status: SystemStatus | null; auth: AuthState; busy: boolean; run: (label: string, action: () => Promise<unknown>) => Promise<void>; logout: () => Promise<void> }) {
  const [name, setName] = useState(status?.hostname?.replace(/\.local$/, "") ?? "labtwin"); const [volume, setVolume] = useState(70);
  return <div className="two-column"><Panel title="设备标识"><form className="form-stack" onSubmit={(event) => { event.preventDefault(); void run("设备设置", () => putJson("/api/v2/settings/device", { name, volume })); }}><label>设备名称<input value={name} maxLength={32} onChange={(event) => setName(event.target.value.replace(/[^a-zA-Z0-9-]/g, ""))} /></label><label>音量 {volume}%<input type="range" min="0" max="100" value={volume} onChange={(event) => setVolume(Number(event.target.value))} /></label><button disabled={busy}>保存设置</button></form></Panel><Panel title="访问与会话"><dl className="detail-list"><div><dt>推荐局域网地址</dt><dd>http://{status?.lan_hostname ?? "labtwin.lan"}/</dd></div><div><dt>mDNS 兼容地址</dt><dd>http://{status?.hostname ?? "labtwin.local"}/</dd></div><div><dt>当前角色</dt><dd>{auth.role}</dd></div><div><dt>设备 ID</dt><dd>{status?.device_id ?? "-"}</dd></div><div><dt>启动 ID</dt><dd>{status?.boot_id ?? "-"}</dd></div></dl><button className="secondary" onClick={() => void logout()}>退出管理员</button></Panel></div>;
}

function AdvancedSettings({ busy, run }: { busy: boolean; run: (label: string, action: () => Promise<unknown>) => Promise<void> }) {
  const [form, setForm] = useState({
    model: "", llm_host: "", api_key: "", volc_speaker: "", volc_api_key: "",
    volc_appkey: "", volc_token: "", volc_asr_cluster: "", proxy_host: "", proxy_port: "",
  });
  const [configured, setConfigured] = useState({
    api_key: false,
    volc_api_key: false,
    volc_token: false,
  });
  const [loadError, setLoadError] = useState("");
  const set = (key: keyof typeof form, value: string) => setForm((current) => ({ ...current, [key]: value }));

  useEffect(() => {
    let cancelled = false;
    void (async () => {
      try {
        const result = await apiRequest<AdvancedSettingsResponse>("/api/v2/settings/advanced");
        if (!result.data || cancelled) return;
        setForm((current) => ({
          ...current,
          model: result.data.model,
          llm_host: result.data.llm_host,
          volc_speaker: result.data.volc_speaker,
          proxy_host: result.data.proxy_host,
          proxy_port: result.data.proxy_port,
          volc_appkey: result.data.volc_appkey,
          volc_asr_cluster: result.data.volc_asr_cluster,
        }));
        setConfigured({
          api_key: result.data.api_key_configured,
          volc_api_key: result.data.volc_api_key_configured,
          volc_token: result.data.volc_token_configured,
        });
      } catch {
        if (!cancelled) setLoadError("无法读取当前配置；仍可填写并保存新的配置。");
      }
    })();
    return () => { cancelled = true; };
  }, []);

  async function save() {
    const result = await putJson("/api/v2/settings/advanced", form);
    setConfigured((current) => ({
      api_key: current.api_key || Boolean(form.api_key),
      volc_api_key: current.volc_api_key || Boolean(form.volc_api_key),
      volc_token: current.volc_token || Boolean(form.volc_token),
    }));
    setForm((current) => ({ ...current, api_key: "", volc_api_key: "", volc_token: "" }));
    return result;
  }

  return <Panel title="AI 与语音设置">
    <p className="section-copy">密钥保存后不会显示原值；留空表示不修改。流式识别使用旧版 AppID、Access Token 和 Cluster，不与 TTS 密钥混用。</p>
    {loadError ? <p className="form-error">{loadError}</p> : null}
    <form className="settings-sections" onSubmit={(event) => { event.preventDefault(); void run("AI 与语音设置", save); }}>
      <fieldset>
        <legend>AI 模型</legend>
        <label>模型<input value={form.model} onChange={(event) => set("model", event.target.value)} /></label>
        <label>服务地址<input value={form.llm_host} onChange={(event) => set("llm_host", event.target.value)} /></label>
        <label>API Key<input type="password" placeholder={configured.api_key ? "已配置；留空表示不修改" : "留空表示不修改"} value={form.api_key} onChange={(event) => set("api_key", event.target.value)} /></label>
      </fieldset>
      <fieldset>
        <legend>语音合成（TTS）</legend>
        <label>音色<input value={form.volc_speaker} onChange={(event) => set("volc_speaker", event.target.value)} /></label>
        <label>语音密钥<input type="password" placeholder={configured.volc_api_key ? "已配置；留空表示不修改" : "留空表示不修改"} value={form.volc_api_key} onChange={(event) => set("volc_api_key", event.target.value)} /></label>
      </fieldset>
      <fieldset>
        <legend>流式语音识别（旧版 ASR）</legend>
        <label>应用 ID<input autoComplete="off" value={form.volc_appkey} onChange={(event) => set("volc_appkey", event.target.value)} /></label>
        <label>Access Token<input type="password" autoComplete="new-password" placeholder={configured.volc_token ? "已配置；留空表示不修改" : "留空表示不修改"} value={form.volc_token} onChange={(event) => set("volc_token", event.target.value)} /></label>
        <label>Cluster<input placeholder="volcengine_streaming_common" value={form.volc_asr_cluster} onChange={(event) => set("volc_asr_cluster", event.target.value)} /></label>
        <p className="section-copy">未填写 Cluster 时，设备使用默认值 <code>volcengine_streaming_common</code>。</p>
      </fieldset>
      <fieldset>
        <legend>代理</legend>
        <label>主机<input value={form.proxy_host} onChange={(event) => set("proxy_host", event.target.value)} /></label>
        <label>端口<input inputMode="numeric" value={form.proxy_port} onChange={(event) => set("proxy_port", event.target.value)} /></label>
      </fieldset>
      <button disabled={busy}>保存 AI 与语音设置</button>
    </form>
  </Panel>;
}

function Skills({ skills, busy, run, reload }: { skills: Skill[]; busy: boolean; run: (label: string, action: () => Promise<unknown>) => Promise<void>; reload: () => Promise<void> }) {
  const [name, setName] = useState("");
  const [content, setContent] = useState("");
  const [revision, setRevision] = useState("");
  const [loading, setLoading] = useState(false);
  const draftKey = (skillName: string) => `labtwin.skill-draft.${skillName}`;

  async function edit(skillName: string) {
    setLoading(true);
    try {
      const result = await apiRequest<SkillDetail>(`/api/v2/skills/${encodeURIComponent(skillName)}`);
      if (!result.data) return;
      let nextContent = result.data.content;
      const cached = localStorage.getItem(draftKey(skillName));
      if (cached) {
        try {
          const draft = JSON.parse(cached) as { revision: string; content: string };
          if (draft.revision === result.data.revision) nextContent = draft.content;
          else localStorage.removeItem(draftKey(skillName));
        } catch {
          localStorage.removeItem(draftKey(skillName));
        }
      }
      setName(result.data.name);
      setRevision(result.data.revision);
      setContent(nextContent);
    } finally {
      setLoading(false);
    }
  }

  function updateContent(value: string) {
    setContent(value);
    if (name) localStorage.setItem(draftKey(name), JSON.stringify({ revision, content: value }));
  }

  function createNew() {
    setName("");
    setContent("");
    setRevision("");
  }

  return <div className="two-column"><Panel title="已安装技能" action={<button className="secondary" onClick={createNew}>新建</button>}><div className="skill-list">{skills.map((skill) => <article key={skill.name}><div><strong>{skill.name}</strong><small>{skill.description || "无描述"} · {formatBytes(skill.size)}</small></div><div className="button-row"><button className="secondary" disabled={busy || loading} onClick={() => void edit(skill.name)}>编辑</button><button className="danger-outline" disabled={busy} onClick={() => void run("删除技能", async () => { await apiRequest(`/api/v2/skills/${encodeURIComponent(skill.name)}`, { method: "DELETE" }); localStorage.removeItem(draftKey(skill.name)); if (name === skill.name) createNew(); await reload(); })}>删除</button></div></article>)}</div></Panel><Panel title={revision ? `编辑 ${name}` : "添加技能"}><form className="form-stack" onSubmit={(event) => { event.preventDefault(); void run("保存技能", async () => { await postJson("/api/v2/skills", { name, content, ...(revision ? { revision } : {}) }); localStorage.removeItem(draftKey(name)); await reload(); await edit(name); }); }}><label>技能名称<input value={name} maxLength={64} required readOnly={Boolean(revision)} onChange={(event) => setName(event.target.value)} /></label><label>Markdown 内容<textarea value={content} maxLength={32768} required rows={16} disabled={loading} onChange={(event) => updateContent(event.target.value)} /></label><button disabled={busy || loading}>{loading ? "正在读取…" : "保存并热加载"}</button></form></Panel></div>;
}

function Logs({ logs, reload }: { logs: LogItem[]; reload: () => Promise<void> }) {
  const [query, setQuery] = useState("");
  const shown = useMemo(() => logs.filter((item) => `${item.level} ${item.tag} ${item.message}`.toLowerCase().includes(query.toLowerCase())), [logs, query]);
  return <Panel title="运行日志" action={<div className="button-row"><input placeholder="过滤日志" value={query} onChange={(event) => setQuery(event.target.value)} /><button className="secondary" onClick={() => void reload()}>刷新</button></div>}><div className="log-view">{shown.map((item, index) => <div key={`${item.epoch}-${index}`}><time>{formatTime(item.epoch)}</time><b className={item.level.toLowerCase()}>{item.level}</b><span>{item.tag}</span><p>{item.message}</p></div>)}</div></Panel>;
}

function Maintenance({ busy, run, status }: { busy: boolean; run: (label: string, action: () => Promise<unknown>) => Promise<void>; status: SystemStatus | null }) {
  const [confirm, setConfirm] = useState(""); const [password, setPassword] = useState("");
  const action = (kind: string, label: string) => run(label, () => postJson(`/api/v2/maintenance/${kind}`, { confirm, password }));
  return <div className="two-column"><Panel title="运行状态"><dl className="detail-list"><div><dt>服务</dt><dd>{status?.service.state ?? "未知"}</dd></div><div><dt>在线客户端</dt><dd>{status?.service.clients ?? 0}</dd></div><div><dt>运行时间</dt><dd>{Math.floor((status?.uptime_seconds ?? 0) / 60)} 分钟</dd></div><div><dt>最大连续堆</dt><dd>{formatBytes(status?.heap_largest_bytes)}</dd></div></dl></Panel><Panel title="维护操作"><div className="form-stack"><label>管理员密码<input type="password" value={password} onChange={(event) => setPassword(event.target.value)} /></label><label>输入 RESTART 或 REBOOT 确认<input value={confirm} onChange={(event) => setConfirm(event.target.value.toUpperCase())} /></label><button className="secondary" disabled={busy || confirm !== "RESTART"} onClick={() => void action("restart", "服务重启")}>重启 ai_agent</button><button className="danger" disabled={busy || confirm !== "REBOOT"} onClick={() => void action("reboot", "整机重启")}>重启板卡</button><button className="secondary" disabled={busy} onClick={() => void run("清理缓存", async () => { await postJson("/api/v2/maintenance/clear-cache", {}); indexedDB.deleteDatabase("labtwin-dashboard"); })}>清理临时缓存</button><p className="warning-copy">清理缓存不会删除实验、Wi‑Fi 或系统配置。</p></div></Panel></div>;
}

function LoginDialog({ auth, close, success }: { auth: AuthState; close: () => void; success: (state: AuthState) => void }) {
  const [password, setPassword] = useState("");
  const [confirm, setConfirm] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const initialSetup = !auth.initialized;

  async function submit(event: React.FormEvent) {
    event.preventDefault();
    setBusy(true);
    setError("");
    try {
      if (initialSetup && password !== confirm) throw new Error("两次密码不一致");
      const result = initialSetup
        ? await postJson<AuthState>("/api/v2/auth/setup", { password })
        : await postJson<AuthState>("/api/v2/auth/login", { password });
      if (result.data) success(result.data);
    } catch (reason) {
      if (reason instanceof ManagementApiError && reason.code === "CREDENTIAL_STORAGE_UNAVAILABLE") {
        setError("板端凭据存储不可用：请重启板端后重试；若仍失败，请更新板端固件。");
      } else if (reason instanceof ManagementApiError && reason.code === "AUTH_CRYPTO_UNAVAILABLE") {
        setError("板端安全服务暂不可用：请重启板端后重试。");
      } else if (reason instanceof ManagementApiError && reason.code === "ADMIN_PASSWORD_INVALID") {
        setError("管理员密码必须是 6 至 128 位数字。");
      } else if (reason instanceof ManagementApiError && reason.code === "ADMIN_ALREADY_INITIALIZED") {
        setError("管理员密码已由其他用户设置，请使用该密码登录。");
      } else {
        setError(reason instanceof Error ? reason.message : "认证失败");
      }
    }
    finally {
      setBusy(false);
    }
  }
  return <div className="dialog-backdrop" onMouseDown={(event) => { if (event.target === event.currentTarget) close(); }}><dialog open><header><div><small>{initialSetup ? "首次设置" : "受保护区域"}</small><h2>{initialSetup ? "设置管理员密码" : "管理员登录"}</h2></div><button aria-label="关闭" onClick={close}>×</button></header><form className="form-stack" onSubmit={submit}>{initialSetup ? <p>首次访问请设置 6 至 128 位数字管理员密码。设置后，后续访问均需登录。</p> : null}<label>管理员密码<input type="password" minLength={initialSetup ? 6 : undefined} maxLength={128} pattern={initialSetup ? "[0-9]{6,128}" : undefined} inputMode={initialSetup ? "numeric" : "text"} autoComplete={initialSetup ? "new-password" : "current-password"} required value={password} onChange={(event) => setPassword(initialSetup ? event.target.value.replace(/\D/g, "") : event.target.value)} /></label>{initialSetup ? <label>确认密码<input type="password" minLength={6} maxLength={128} pattern="[0-9]{6,128}" inputMode="numeric" autoComplete="new-password" required value={confirm} onChange={(event) => setConfirm(event.target.value.replace(/\D/g, ""))} /></label> : null}{error ? <p className="form-error">{error}</p> : null}<button disabled={busy}>{busy ? "正在验证…" : initialSetup ? "设置密码" : "登录"}</button></form></dialog></div>;
}
