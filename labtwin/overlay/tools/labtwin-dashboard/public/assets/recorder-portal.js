(() => {
  const root = document.createElement('div');
  let panel;
  let opened = false;
  let refreshTimer;
  let writing = false;

  const requestId = () => `recording-${Date.now()}-${Math.random().toString(36).slice(2, 11)}`;
  const seconds = value => {
    const total = Math.max(0, Math.floor(Number(value) || 0));
    const hours = Math.floor(total / 3600);
    const minutes = Math.floor((total % 3600) / 60);
    const secs = total % 60;
    return hours ? `${hours}:${String(minutes).padStart(2, '0')}:${String(secs).padStart(2, '0')}` : `${minutes}:${String(secs).padStart(2, '0')}`;
  };
  const bytes = value => {
    const n = Number(value) || 0;
    return n >= 1024 * 1024 ? `${(n / 1024 / 1024).toFixed(1)} MB` : `${(n / 1024).toFixed(1)} KB`;
  };
  const date = value => Number(value) > 1600000000 ? new Date(Number(value) * 1000).toLocaleString() : '时间未同步';

  async function api(path, method = 'GET') {
    const auth = await fetch('/api/v2/auth/state', { credentials: 'same-origin', cache: 'no-store' }).then(r => r.json());
    const headers = { 'X-Request-ID': requestId() };
    if (method !== 'GET') headers['X-CSRF-Token'] = auth.data?.csrf_token || '';
    const response = await fetch(path, { method, headers, credentials: 'same-origin', cache: 'no-store' });
    const body = await response.json();
    if (!response.ok || body.error) throw new Error(body.error?.message || `请求失败 (${response.status})`);
    return body.data;
  }

  function field(selector) { return panel.querySelector(selector); }
  function showError(message = '') { field('[data-rec-error]').textContent = message; }

  function render(data) {
    const status = field('[data-rec-status]');
    const start = field('[data-rec-start]');
    const stop = field('[data-rec-stop]');
    const current = data.current;
    status.classList.toggle('active', Boolean(data.active));
    status.innerHTML = data.active
      ? `<strong>● 正在录音</strong><p>已录制 ${seconds(current.duration_seconds)}，本次最多 ${seconds(current.max_duration_seconds)}。录音期间语音唤醒已暂停。</p>`
      : '<strong>● 等待录音</strong><p>开始录音后，语音唤醒会暂时暂停；结束后自动恢复。</p>';
    start.disabled = writing || data.active || Number(data.available_duration_seconds) < 1;
    stop.disabled = writing || !data.active || data.state === 'STARTING' || data.state === 'FINALIZING';
    const reason = data.end_reason === 'publish_failed' ? '录音文件保存失败，暂存文件已保留，请检查存储并重试恢复。' :
      data.end_reason === 'capture_error' || data.end_reason === 'storage_error' ? '录音异常结束，已保存可回放的部分录音。' :
      Number(data.available_duration_seconds) < 1 ? '录音配额或可用空间不足，请删除历史录音后重试。' : '';
    if (!writing) showError(reason);
    field('[data-rec-summary]').innerHTML = `<span>已用 <b>${bytes(data.used_bytes)}</b> / ${bytes(data.quota_bytes)}</span><span>可录 <b>${seconds(data.available_duration_seconds)}</b></span><span>系统可用 <b>${bytes(data.free_bytes)}</b></span>`;
    const list = field('[data-rec-list]');
    const existing = new Map(Array.from(list.querySelectorAll('article')).map(row => [row.dataset.id, row]));
    const recordings = Array.isArray(data.recordings) ? [...data.recordings].sort((a, b) =>
      Number(b.modified_epoch) - Number(a.modified_epoch) || b.id.localeCompare(a.id)) : [];
    field('[data-rec-history]').textContent = `历史录音（${recordings.length} 条）`;
    field('[data-rec-history-jump]').textContent = `查看历史（${recordings.length} 条）`;
    field('[data-rec-history-jump]').disabled = recordings.length === 0;
    field('[data-rec-history-hint]').textContent = recordings.length > 1 ? '最新录音在前，可向下滚动查看全部录音。' : '';
    if (!recordings.length) {
      list.textContent = '暂无已完成的录音。';
      return;
    }
    for (const [id, row] of existing) if (!recordings.some(item => item.id === id)) row.remove();
    if (!list.querySelector('article')) list.replaceChildren();
    for (const [index, item] of recordings.entries()) {
      // Polling must not recreate audio elements and restart ongoing playback.
      if (existing.has(item.id)) {
        existing.get(item.id).querySelector('small').textContent = `${date(item.modified_epoch)} · ${seconds(item.duration_seconds)} · ${bytes(item.size_bytes)}${item.end_reason ? ` · ${item.end_reason}` : ''}`;
        existing.get(item.id).style.order = index;
        continue;
      }
      const row = document.createElement('article');
      row.dataset.id = item.id;
      row.className = 'labtwin-recording-item';
      row.style.order = index;
      const meta = document.createElement('div');
      meta.className = 'labtwin-recording-meta';
      const title = document.createElement('strong');
      title.textContent = item.id;
      const detail = document.createElement('small');
      detail.textContent = `${date(item.modified_epoch)} · ${seconds(item.duration_seconds)} · ${bytes(item.size_bytes)}${item.end_reason ? ` · ${item.end_reason}` : ''}`;
      meta.append(title, detail);
      const controls = document.createElement('div');
      controls.className = 'labtwin-recording-controls';
      const audio = document.createElement('audio');
      audio.controls = true;
      audio.preload = 'metadata';
      audio.src = `/api/v2/recordings/${encodeURIComponent(item.id)}/audio`;
      const remove = document.createElement('button');
      remove.textContent = '删除';
      remove.addEventListener('click', async () => {
        if (!confirm(`删除 ${item.id}？此操作不可恢复。`)) return;
        await mutate(`/api/v2/recordings/${encodeURIComponent(item.id)}`, 'DELETE');
      });
      controls.append(audio, remove);
      row.append(meta, controls);
      // Insert only the new row; moving an existing audio node can interrupt playback.
      const nextRow = recordings.slice(index + 1).map(recording => existing.get(recording.id)).find(Boolean);
      list.insertBefore(row, nextRow || null);
      existing.set(item.id, row);
    }
  }

  async function refresh() {
    if (!opened) return;
    try { render(await api('/api/v2/recordings')); }
    catch (error) { showError(error.message); }
    finally { clearTimeout(refreshTimer); refreshTimer = setTimeout(refresh, 1000); }
  }

  async function mutate(path, method) {
    writing = true;
    showError('正在处理…');
    try { render(await api(path, method)); showError(''); }
    catch (error) { showError(error.message); }
    finally { writing = false; refresh(); }
  }

  function open() {
    if (!panel) {
      panel = document.createElement('div');
      panel.className = 'labtwin-recorder-overlay';
      panel.innerHTML = `<section class="labtwin-recorder-panel" role="dialog" aria-modal="true" aria-label="录音与回放"><header class="labtwin-recorder-head"><div><h2>录音与回放</h2><p>管理员专用 · 16 kHz 单声道 WAV · 单条最长一小时</p></div><button class="labtwin-recorder-close" aria-label="关闭">×</button></header><main class="labtwin-recorder-body"><div class="labtwin-recorder-status" data-rec-status></div><div class="labtwin-recorder-actions"><button data-rec-start>开始录音</button><button class="stop" data-rec-stop>停止录音</button><button class="history" data-rec-history-jump disabled>查看历史（0 条）</button></div><p class="labtwin-recorder-error" data-rec-error></p><div class="labtwin-recorder-summary" data-rec-summary></div><h3 data-rec-history>历史录音（0 条）</h3><p class="labtwin-recorder-history-hint" data-rec-history-hint></p><div class="labtwin-recorder-list" data-rec-list></div></main></section>`;
      panel.querySelector('.labtwin-recorder-close').addEventListener('click', close);
      panel.addEventListener('click', event => { if (event.target === panel) close(); });
      panel.querySelector('[data-rec-start]').addEventListener('click', () => mutate('/api/v2/recordings', 'POST'));
      panel.querySelector('[data-rec-history-jump]').addEventListener('click', () => {
        field('[data-rec-history]').scrollIntoView({ block: 'start', behavior: 'smooth' });
      });
      panel.querySelector('[data-rec-stop]').addEventListener('click', async () => {
        const data = await api('/api/v2/recordings');
        if (data.current?.id) await mutate(`/api/v2/recordings/${encodeURIComponent(data.current.id)}/stop`, 'POST');
      });
    }
    document.body.append(panel);
    opened = true;
    refresh();
  }
  function close() { opened = false; clearTimeout(refreshTimer); panel?.remove(); }

  const launch = document.createElement('button');
  launch.id = 'labtwin-recorder-launcher';
  launch.textContent = '● 录音';
  launch.addEventListener('click', open);
  document.addEventListener('DOMContentLoaded', () => document.body.append(root, launch), { once: true });
})();
