import Dexie, { type EntityTable } from "dexie";
import { snapshotSchema, type DashboardSnapshot, type StreamEvent } from "../protocol/schema";

type SnapshotRecord = {
  deviceId: string;
  savedAt: number;
  snapshot: DashboardSnapshot;
};

type EventRecord = StreamEvent & {
  key: string;
  deviceId: string;
};

class LabTwinDatabase extends Dexie {
  snapshots!: EntityTable<SnapshotRecord, "deviceId">;
  events!: EntityTable<EventRecord, "key">;

  constructor() {
    super("labtwin-dashboard");
    this.version(1).stores({
      snapshots: "&deviceId,savedAt",
      events: "&key,deviceId,[deviceId+stream+subject_id+seq],timestamp_epoch",
    });
  }
}

let database: LabTwinDatabase | null = null;

function getDatabase() {
  if (typeof indexedDB === "undefined") return null;
  database ??= new LabTwinDatabase();
  return database;
}

export async function saveSnapshot(snapshot: DashboardSnapshot) {
  const db = getDatabase();
  if (!db) return;
  await db.snapshots.put({ deviceId: snapshot.device.device_id, savedAt: Date.now(), snapshot });
}

export async function loadSnapshot(deviceId: string) {
  const db = getDatabase();
  if (!db) return null;
  const record = await db.snapshots.get(deviceId);
  if (!record) return null;
  const parsed = snapshotSchema.safeParse(record.snapshot);
  if (parsed.success) return parsed.data;
  await db.snapshots.delete(deviceId);
  return null;
}

export async function loadLatestSnapshot() {
  const db = getDatabase();
  if (!db) return null;
  const record = await db.snapshots.orderBy("savedAt").last();
  if (!record) return null;
  const parsed = snapshotSchema.safeParse(record.snapshot);
  if (parsed.success) return parsed.data;
  await db.snapshots.delete(record.deviceId);
  return null;
}

export async function clearSnapshotCache() {
  const db = getDatabase();
  if (!db) return;
  await db.transaction("rw", db.snapshots, db.events, async () => {
    await db.snapshots.clear();
    await db.events.clear();
  });
}
