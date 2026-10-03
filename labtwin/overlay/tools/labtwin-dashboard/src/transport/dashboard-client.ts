import {
  responseEnvelopeSchema,
  snapshotSchema,
  streamEventSchema,
  type CursorVector,
  type DashboardSnapshot,
  type StreamEvent,
} from "../protocol/schema";
import { portableUuid } from "../crypto/portable";

export type ConnectionState =
  | "demo"
  | "connecting"
  | "syncing"
  | "online"
  | "offline"
  | "error";

type ClientOptions = {
  endpoint: string;
  token?: string;
  cursor?: CursorVector;
  onState: (state: ConnectionState, detail?: string) => void;
  onSnapshot: (snapshot: DashboardSnapshot) => void;
  onEvent: (event: StreamEvent) => void;
};

const HEARTBEAT_INTERVAL_MS = 15_000;
const HEARTBEAT_TIMEOUT_MS = 45_000;

export class DashboardClient {
  private socket: WebSocket | null = null;
  private reconnectTimer: number | null = null;
  private reconnectAttempt = 0;
  private closed = false;
  private lastReceivedAt = 0;
  private heartbeatTimer: number | null = null;
  private cursorCleared = false;

  constructor(private readonly options: ClientOptions) {}

  connect() {
    this.closed = false;
    this.stopHeartbeat();
    if (this.options.endpoint.startsWith("demo://")) {
      this.options.onState("demo", "正在使用脱敏模拟数据");
      return;
    }

    this.options.onState("connecting", this.options.endpoint);
    try {
      this.socket = new WebSocket(this.options.endpoint, ["labtwin.dashboard.v1"]);
    } catch (error) {
      this.fail(error instanceof Error ? error.message : "无法创建 WebSocket");
      return;
    }

    this.socket.onopen = () => {
      this.lastReceivedAt = Date.now();
      this.startHeartbeat();
      this.options.onState("syncing", "连接成功，正在校准游标");
      const cursor = this.cursorCleared ? null : this.options.cursor;
      this.send("hello", {
        client: "labtwin-dashboard",
        token: this.options.token || undefined,
        cursor,
      });
      this.send("sync", { cursor: cursor ?? null, page_size: 128 });
    };

    this.socket.onmessage = (message) => this.handleMessage(message.data);
    this.socket.onerror = () => this.options.onState("error", "连接发生错误");
    this.socket.onclose = () => {
      this.socket = null;
      this.stopHeartbeat();
      if (!this.closed) this.scheduleReconnect();
    };
  }

  disconnect() {
    this.closed = true;
    this.stopHeartbeat();
    if (this.reconnectTimer !== null) window.clearTimeout(this.reconnectTimer);
    this.socket?.close(1000, "dashboard closed");
    this.socket = null;
    this.options.onState("offline", "已主动断开");
  }

  private send(op: string, payload: unknown) {
    if (this.socket?.readyState !== WebSocket.OPEN) return;
    this.socket.send(
      JSON.stringify({
        protocol: "labtwin.dashboard.v1",
        message_id: portableUuid(),
        kind: "request",
        op,
        schema_version: 1,
        payload,
      }),
    );
  }

  private handleMessage(raw: unknown) {
    this.lastReceivedAt = Date.now();
    try {
      const text = typeof raw === "string" ? raw : String(raw);
      const envelope = responseEnvelopeSchema.parse(JSON.parse(text));
      if (envelope.kind === "error") {
        if (envelope.op === "cursor_expired") {
          this.cursorCleared = true;
          this.options.onState("syncing", "游标过期，正在全量同步");
          this.send("sync", { cursor: null, page_size: 128 });
          return;
        }
        this.options.onState("error", `板端拒绝请求：${envelope.op}`);
        return;
      }
      if (envelope.op === "sync") {
        this.options.onSnapshot(snapshotSchema.parse(envelope.payload));
        this.cursorCleared = false;
        this.reconnectAttempt = 0;
        this.options.onState("online", "游标一致，正在接收增量事件");
      } else if (envelope.kind === "event") {
        this.options.onEvent(streamEventSchema.parse(envelope.payload));
      }
      // pong / hello_ack / sync_page / sync_end：更新 lastReceivedAt 即可
    } catch (error) {
      this.options.onState(
        "error",
        error instanceof Error ? `协议校验失败：${error.message}` : "协议校验失败",
      );
    }
  }

  private startHeartbeat() {
    this.stopHeartbeat();
    this.lastReceivedAt = Date.now();
    if (typeof window === "undefined" || typeof window.setInterval !== "function") return;
    this.heartbeatTimer = window.setInterval(() => {
      if (this.socket?.readyState !== WebSocket.OPEN) return;
      if (Date.now() - this.lastReceivedAt > HEARTBEAT_TIMEOUT_MS) {
        this.options.onState("offline", "心跳超时，正在重连");
        this.socket.close(4000, "heartbeat timeout");
        return;
      }
      this.send("ping", {});
    }, HEARTBEAT_INTERVAL_MS);
  }

  private stopHeartbeat() {
    if (this.heartbeatTimer !== null) {
      if (typeof window !== "undefined" && typeof window.clearInterval === "function") {
        window.clearInterval(this.heartbeatTimer);
      }
      this.heartbeatTimer = null;
    }
  }

  private scheduleReconnect() {
    const seconds = Math.min(15, 2 ** this.reconnectAttempt);
    const delay = seconds * 1000 + Math.floor(Math.random() * 350);
    this.reconnectAttempt += 1;
    this.options.onState("offline", `${Math.round(delay / 1000)} 秒后重连`);
    this.reconnectTimer = window.setTimeout(() => this.connect(), delay);
  }

  private fail(detail: string) {
    this.options.onState("error", detail);
    if (!this.closed) this.scheduleReconnect();
  }
}
