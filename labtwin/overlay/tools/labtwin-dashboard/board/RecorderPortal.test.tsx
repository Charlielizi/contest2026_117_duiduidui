// @vitest-environment jsdom
import { fireEvent, waitFor } from "@testing-library/react";
import { afterEach, expect, it, vi } from "vitest";
import source from "../public/assets/recorder-portal.js?raw";
let active = false;
const recorded = { id: "rec-000001", duration_seconds: 30, size_bytes: 960044, modified_epoch: 1790000000 };
let recordings = [recorded];
function data() { return { active, state: active ? "RECORDING" : "IDLE", current: active ? { id: recorded.id, duration_seconds: 1, max_duration_seconds: 3600 } : undefined,
  available_duration_seconds: 3600, quota_bytes: 125829120, used_bytes: 960044, free_bytes: 200000000, recordings }; }
afterEach(() => {
  document.querySelector<HTMLButtonElement>('.labtwin-recorder-close')?.click();
  document.body.replaceChildren(); vi.unstubAllGlobals(); vi.restoreAllMocks();
  recordings = [recorded]; active = false;
});

function mountHistory() {
  vi.stubGlobal('fetch', vi.fn(async (path: string) => ({ ok: true,
    json: async () => ({ data: path.endsWith('/auth/state') ? { csrf_token: 'test-csrf' } : data() }) })));
  window.eval(source);
  document.dispatchEvent(new Event('DOMContentLoaded'));
  document.querySelector<HTMLButtonElement>('#labtwin-recorder-launcher')!.click();
}

it('shows every recording newest-first, displays a count and provides a history jump', async () => {
  recordings = [recorded, { ...recorded, id: 'rec-000003', modified_epoch: 1790000020 },
    { ...recorded, id: 'rec-000002', modified_epoch: 1790000010 }];
  const inputIds = recordings.map(item => item.id);
  mountHistory();
  await waitFor(() => expect(document.querySelectorAll('[data-rec-list] article')).toHaveLength(3));
  expect(Array.from(document.querySelectorAll('[data-rec-list] article')).map(row => (row as HTMLElement).dataset.id))
    .toEqual(['rec-000003', 'rec-000002', 'rec-000001']);
  expect(recordings.map(item => item.id)).toEqual(inputIds);
  expect(document.querySelector('[data-rec-history]')?.textContent).toBe('历史录音（3 条）');
  expect(document.querySelector('[data-rec-history-hint]')?.textContent).toContain('滚动');
  const heading = document.querySelector('[data-rec-history]') as HTMLElement;
  const scroll = vi.fn(); heading.scrollIntoView = scroll;
  fireEvent.click(document.querySelector('[data-rec-history-jump]')!);
  expect(scroll).toHaveBeenCalledWith({ block: 'start', behavior: 'smooth' });
});

it('inserts new history above existing rows without replacing or moving the existing audio', async () => {
  mountHistory();
  await waitFor(() => expect(document.querySelector('audio')).not.toBeNull());
  const audio = document.querySelector('audio');
  const row = document.querySelector('[data-rec-list] article') as HTMLElement;
  const remove = vi.spyOn(row, 'remove');
  recordings = [recorded, { ...recorded, id: 'rec-000002', modified_epoch: 1790000010 }];
  await waitFor(() => expect(document.querySelectorAll('[data-rec-list] article')).toHaveLength(2), { timeout: 2000 });
  expect(document.querySelector('[data-rec-list] article')?.getAttribute('data-id')).toBe('rec-000002');
  expect(row.querySelector('audio')).toBe(audio);
  expect(remove).not.toHaveBeenCalled();
  expect(document.querySelector('[data-rec-history]')?.textContent).toBe('历史录音（2 条）');
});

it('shows an empty history without an enabled jump button', async () => {
  recordings = [];
  mountHistory();
  await waitFor(() => expect(document.querySelector('[data-rec-list]')?.textContent).toBe('暂无已完成的录音。'));
  expect(document.querySelector('[data-rec-history]')?.textContent).toBe('历史录音（0 条）');
  expect((document.querySelector('[data-rec-history-jump]') as HTMLButtonElement).disabled).toBe(true);
});
it("keeps the audio element during polling and protects recording writes with CSRF", async () => {
  active = false;
  const fetchMock = vi.fn(async (path: string, init?: RequestInit) => {
    if (path.endsWith('/auth/state')) return { ok: true, json: async () => ({ data: { csrf_token: 'test-csrf' } }) };
    if (init?.method === 'POST') active = !path.endsWith('/stop');
    return { ok: true, json: async () => ({ data: data() }) };
  });
  vi.stubGlobal('fetch', fetchMock);
  window.eval(source);
  document.dispatchEvent(new Event('DOMContentLoaded'));
  document.querySelector<HTMLButtonElement>('#labtwin-recorder-launcher')!.click();
  await waitFor(() => expect(document.querySelector('audio')).not.toBeNull());
  const audio = document.querySelector('audio');
  await new Promise((resolve) => window.setTimeout(resolve, 1100));
  expect(document.querySelector('audio')).toBe(audio);
  expect(audio?.getAttribute('src')).toBe('/api/v2/recordings/rec-000001/audio');
  fireEvent.click(document.querySelector('[data-rec-start]')!);
  await waitFor(() => expect(active).toBe(true));
  expect(fetchMock).toHaveBeenCalledWith('/api/v2/recordings', expect.objectContaining({ method: 'POST', headers: expect.objectContaining({ 'X-CSRF-Token': 'test-csrf', 'X-Request-ID': expect.any(String) }) }));
  await waitFor(() => expect((document.querySelector('[data-rec-stop]') as HTMLButtonElement).disabled).toBe(false));
  fireEvent.click(document.querySelector('[data-rec-stop]')!);
  await waitFor(() => expect(active).toBe(false));
});
