import { afterEach, describe, expect, it, vi } from "vitest";
import { apiRequest } from "./api";
import { loadExperimentHistory } from "./experiment-history";
vi.mock("./api", () => ({ apiRequest: vi.fn() }));
afterEach(() => vi.resetAllMocks());
const page = (ids: string[], next: number | null) => ({ request_id: "test", revision: 1, error: null, data: { experiments: ids.map((experiment_id) => ({ experiment_id })), next_offset: next } });
describe("complete experiment history", () => {
  it("reads beyond 32 records and deduplicates ids across changing pages", async () => {
    vi.mocked(apiRequest).mockResolvedValueOnce(page(Array.from({ length: 32 }, (_, index) => `EXP-${index}`), 32)).mockResolvedValueOnce(page(["EXP-31", "EXP-32", "EXP-33"], null));
    const result = await loadExperimentHistory();
    expect(result).toHaveLength(34);
    expect(result[33].experiment_id).toBe("EXP-33");
    expect(apiRequest).toHaveBeenLastCalledWith("/api/v2/data/experiments?limit=32&offset=32", { signal: undefined });
  });
  it("does not silently reuse fallback tasks when the board history is empty", async () => {
    vi.mocked(apiRequest).mockResolvedValueOnce(page([], null));
    expect(await loadExperimentHistory()).toEqual([]);
  });
  it("rejects non-advancing pagination instead of looping forever", async () => {
    vi.mocked(apiRequest).mockResolvedValueOnce(page(["EXP-1"], 0));
    await expect(loadExperimentHistory()).rejects.toThrow("分页异常");
  });
  it("aborts before fetching after calendar navigation/unmount", async () => {
    const controller = new AbortController(); controller.abort();
    await expect(loadExperimentHistory(controller.signal)).rejects.toHaveProperty("name", "AbortError");
    expect(apiRequest).not.toHaveBeenCalled();
  });
});
