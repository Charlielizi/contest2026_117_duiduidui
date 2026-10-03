import { afterEach, describe, expect, it, vi } from "vitest";
import { demoSnapshot } from "../data/demo";
import { DashboardClient } from "./dashboard-client";

class FakeWebSocket {
  static OPEN = 1;
  static latest: FakeWebSocket;
  readyState = FakeWebSocket.OPEN;
  sent: string[] = [];
  onopen: (() => void) | null = null;
  onmessage: ((message: { data: string }) => void) | null = null;
  onerror: (() => void) | null = null;
  onclose: (() => void) | null = null;

  constructor(
    public readonly url: string,
    public readonly protocols: string[],
  ) {
    FakeWebSocket.latest = this;
  }

  send(message: string) {
    this.sent.push(message);
  }

  close() {}
}

describe("DashboardClient", () => {
  afterEach(() => vi.restoreAllMocks());

  it("performs hello and cursor sync before accepting events", () => {
    Object.defineProperty(globalThis, "WebSocket", {
      value: FakeWebSocket,
      configurable: true,
    });
    Object.defineProperty(globalThis, "window", {
      value: { setTimeout, clearTimeout },
      configurable: true,
    });

    const states: string[] = [];
    const snapshots: string[] = [];
    const client = new DashboardClient({
      endpoint: "ws://board/labtwin",
      cursor: demoSnapshot.cursor,
      onState: (state) => states.push(state),
      onSnapshot: (snapshot) => snapshots.push(snapshot.device.device_id),
      onEvent: vi.fn(),
    });

    client.connect();
    FakeWebSocket.latest.onopen?.();

    expect(FakeWebSocket.latest.protocols).toEqual(["labtwin.dashboard.v1"]);
    expect(FakeWebSocket.latest.sent.map((item) => JSON.parse(item).op)).toEqual([
      "hello",
      "sync",
    ]);

    FakeWebSocket.latest.onmessage?.({
      data: JSON.stringify({
        protocol: "labtwin.dashboard.v1",
        message_id: "board-1",
        kind: "response",
        op: "sync",
        schema_version: 1,
        payload: demoSnapshot,
      }),
    });

    expect(states).toEqual(["connecting", "syncing", "online"]);
    expect(snapshots).toEqual(["gemini-s1-lab-01"]);
  });

  it("clears cursor and re-syncs on cursor_expired", () => {
    Object.defineProperty(globalThis, "WebSocket", {
      value: FakeWebSocket,
      configurable: true,
    });
    Object.defineProperty(globalThis, "window", {
      value: { setTimeout, clearTimeout },
      configurable: true,
    });

    const client = new DashboardClient({
      endpoint: "ws://board/labtwin",
      cursor: demoSnapshot.cursor,
      onState: () => {},
      onSnapshot: () => {},
      onEvent: vi.fn(),
    });

    client.connect();
    FakeWebSocket.latest.onopen?.();

    FakeWebSocket.latest.onmessage?.({
      data: JSON.stringify({
        protocol: "labtwin.dashboard.v1",
        message_id: "board-err",
        kind: "error",
        op: "cursor_expired",
        schema_version: 1,
        payload: null,
      }),
    });

    const sent = FakeWebSocket.latest.sent.map((item) => JSON.parse(item));
    const lastSync = sent[sent.length - 1];
    expect(lastSync.op).toBe("sync");
    expect(lastSync.payload.cursor).toBeNull();
  });
});
