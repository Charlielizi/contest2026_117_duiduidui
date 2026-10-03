import { afterEach, describe, expect, it, vi } from "vitest";
import { apiRequest } from "./api";
afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); });
describe("bounded management requests", () => {
  it("times out a hanging write explicitly without retrying it", async () => {
    vi.useFakeTimers();
    const fetcher = vi.fn((_path, init) => new Promise((_resolve, reject) => {
      init.signal.addEventListener("abort", () => reject(new DOMException("aborted", "AbortError")));
    }));
    vi.stubGlobal("fetch", fetcher);
    const result = apiRequest("/api/v2/agent/requests", { method: "POST", body: "{}" });
    const rejected = expect(result).rejects.toHaveProperty("code", "REQUEST_TIMEOUT");
    await vi.advanceTimersByTimeAsync(15_000); await rejected;
    expect(fetcher).toHaveBeenCalledTimes(1);
  });
  it("honors an already cancelled caller without waiting for timeout", async () => {
    vi.stubGlobal("fetch", vi.fn(async (_path, init) => {
      if (init.signal.aborted) throw new DOMException("aborted", "AbortError");
    }));
    const controller = new AbortController(); controller.abort();
    await expect(apiRequest("/test", { signal: controller.signal })).rejects.toHaveProperty("name", "AbortError");
  });
});
