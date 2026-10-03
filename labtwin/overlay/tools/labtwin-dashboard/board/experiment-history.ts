import type { Experiment } from "../src/protocol/schema";
import { apiRequest } from "./api";

type Index = { experiments: Experiment[]; next_offset?: number | null };

export async function loadExperimentHistory(signal?: AbortSignal) {
  const items = new Map<string, Experiment>();
  let offset = 0;
  for (;;) {
    if (signal?.aborted) throw new DOMException("取消读取", "AbortError");
    const response = await apiRequest<Index>(`/api/v2/data/experiments?limit=32&offset=${offset}`, { signal });
    if (!response.data) throw new Error("实验历史响应为空");
    for (const item of response.data.experiments) items.set(item.experiment_id, item);
    const next = response.data.next_offset;
    if (next === null || next === undefined) return [...items.values()];
    if (!Number.isSafeInteger(next) || next <= offset || response.data.experiments.length === 0) {
      throw new Error("实验历史分页异常，请刷新重试");
    }
    offset = next;
  }
}
