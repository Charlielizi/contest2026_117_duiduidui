export type ApiError = {
  code: string;
  message: string;
};

export type ApiEnvelope<T> = {
  request_id: string;
  revision: number;
  data: T | null;
  error: ApiError | null;
};

export class ManagementApiError extends Error {
  constructor(
    message: string,
    readonly status: number,
    readonly code = "HTTP_ERROR",
  ) {
    super(message);
  }
}

let csrfToken = "";

export function setCsrfToken(token: string) {
  csrfToken = token;
}

export async function apiRequest<T>(
  path: string,
  init: RequestInit = {},
): Promise<ApiEnvelope<T>> {
  const method = (init.method ?? "GET").toUpperCase();
  const headers = new Headers(init.headers);
  headers.set("Accept", "application/json");
  if (init.body && !headers.has("Content-Type")) {
    headers.set("Content-Type", "application/json");
  }
  if (method !== "GET" && method !== "HEAD") {
    headers.set("X-Request-ID", portableUuid());
    if (csrfToken) headers.set("X-CSRF-Token", csrfToken);
  }

  const controller = new AbortController();
  const abort = () => controller.abort(init.signal?.reason);
  if (init.signal?.aborted) abort();
  else init.signal?.addEventListener("abort", abort, { once: true });
  let timedOut = false;
  const timeout = setTimeout(() => { timedOut = true; controller.abort(); }, 15_000);
  try {
    const response = await fetch(path, {
      ...init, headers, signal: controller.signal,
      credentials: "same-origin", cache: "no-store",
    });
    const envelope = (await response.json()) as ApiEnvelope<T>;
    if (!response.ok || envelope.error) {
      throw new ManagementApiError(envelope.error?.message ?? `请求失败 (${response.status})`,
        response.status, envelope.error?.code);
    }
    return envelope;
  } catch (error) {
    if (timedOut) throw new ManagementApiError("请求超时；写操作可能已生效，请先核对状态，不要重复操作。", 0, "REQUEST_TIMEOUT");
    throw error;
  } finally {
    clearTimeout(timeout);
    init.signal?.removeEventListener("abort", abort);
  }
}

export function postJson<T>(path: string, value: unknown) {
  return apiRequest<T>(path, { method: "POST", body: JSON.stringify(value) });
}

export function putJson<T>(path: string, value: unknown, revision?: number) {
  const headers = revision === undefined ? undefined : { "If-Match": String(revision) };
  return apiRequest<T>(path, {
    method: "PUT",
    headers,
    body: JSON.stringify(value),
  });
}
import { portableUuid } from "../src/crypto/portable";
