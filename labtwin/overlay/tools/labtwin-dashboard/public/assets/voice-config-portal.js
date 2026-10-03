(() => {
  'use strict';

  const endpoint = '/api/v2/settings/advanced';
  const requestId = () => `voice-config-${Date.now()}-${Math.random().toString(36).slice(2, 11)}`;
  let mounted = false;

  async function api(method = 'GET', body) {
    const headers = { 'X-Request-ID': requestId() };
    if (method !== 'GET') {
      const auth = await fetch('/api/v2/auth/state', {
        credentials: 'same-origin', cache: 'no-store'
      }).then(response => response.json());
      headers['X-CSRF-Token'] = auth.data?.csrf_token || '';
      headers['Content-Type'] = 'application/json';
    }
    const response = await fetch(endpoint, {
      method, headers, credentials: 'same-origin', cache: 'no-store',
      body: body ? JSON.stringify(body) : undefined
    });
    const payload = await response.json();
    if (!response.ok || payload.error) {
      throw new Error(payload.error?.message || `请求失败 (${response.status})`);
    }
    return payload.data || {};
  }

  function createField(label, name, options = {}) {
    const field = document.createElement('label');
    field.textContent = label;
    const input = document.createElement('input');
    input.name = name;
    input.type = options.type || 'text';
    input.autocomplete = options.autocomplete || 'off';
    input.maxLength = options.maxLength || 127;
    if (options.placeholder) input.placeholder = options.placeholder;
    field.append(input);
    return field;
  }

  function setField(form, name, value, placeholder) {
    const input = form.elements.namedItem(name);
    if (!input) return;
    if (value) input.value = value;
    if (placeholder) input.placeholder = placeholder;
  }

  function syncReactInput(input, value) {
    if (!input || !value) return;
    input.value = value;
    input.dispatchEvent(new Event('input', { bubbles: true }));
  }

  async function load(form, status) {
    status.textContent = '正在读取语音配置…';
    try {
      const data = await api();
      setField(form, 'volc_appkey', data.volc_appkey);
      setField(form, 'volc_cluster', data.volc_cluster, '未设置时使用 volcano_tts');
      setField(form, 'volc_speaker', data.volc_speaker, 'zh_male_beijingxiaoye_emo_v2_mars_bigtts');
      setField(form, 'volc_asr_cluster', data.volc_asr_cluster, 'volcengine_streaming_common');
      setField(form, 'volc_token', '', data.volc_token_configured ? '已配置；留空表示不修改' : 'Access Token');
      setField(form, 'volc_api_key', '', data.volc_api_key_configured ? '已配置；留空表示不修改' : '仅旧 HTTP TTS 兼容路径使用');
      status.textContent = '实时 TTS 与旧版 ASR 共用 AppID 和 Access Token。';
    } catch (error) {
      status.textContent = `无法读取配置：${error.message}`;
      status.classList.add('form-error');
    }
  }

  function mount() {
    if (mounted) return;
    const legacyAsr = [...document.querySelectorAll('fieldset')]
      .find(fieldset => fieldset.querySelector('legend')?.textContent.includes('流式语音识别'));
    const legacyTts = [...document.querySelectorAll('fieldset')]
      .find(fieldset => fieldset.querySelector('legend')?.textContent.includes('语音合成'));
    const form = legacyAsr?.closest('form');
    if (!form || !legacyTts) return;

    mounted = true;
    const legacyTtsInputs = legacyTts.querySelectorAll('input');
    const legacyAsrInputs = legacyAsr.querySelectorAll('input');
    legacyTts.hidden = true;
    legacyAsr.hidden = true;
    for (const input of [...legacyTts.querySelectorAll('[name]'), ...legacyAsr.querySelectorAll('[name]')]) {
      input.removeAttribute('name');
    }
    const group = document.createElement('fieldset');
    group.className = 'labtwin-voice-config';
    group.innerHTML = '<legend>火山引擎语音服务</legend><p class="section-copy">实时语音合成（TTS V1）与旧版流式识别（ASR V2）使用同一应用的 AppID 与 Access Token。保存后不会显示密钥原值；留空不会覆盖已有密钥。</p>';
    group.append(
      createField('应用 ID（AppID）', 'volc_appkey', { maxLength: 63 }),
      createField('Access Token（TTS / ASR 共用）', 'volc_token', { type: 'password', autocomplete: 'new-password', maxLength: 127 }),
      createField('TTS Cluster', 'volc_cluster', { placeholder: 'volcano_tts', maxLength: 63 }),
      createField('TTS 音色（voice_type）', 'volc_speaker', { maxLength: 63 }),
      createField('ASR Cluster', 'volc_asr_cluster', { placeholder: 'volcengine_streaming_common', maxLength: 127 }),
      createField('旧 HTTP TTS API Key（可选）', 'volc_api_key', { type: 'password', autocomplete: 'new-password', maxLength: 127 })
    );
    const hint = document.createElement('p');
    hint.className = 'labtwin-voice-config-hint';
    hint.textContent = '旧 HTTP TTS API Key 不用于当前实时播报；实时播报使用上方的 Access Token。ASR V3 Resource ID 不在此处配置，因为当前固件尚未实现 V3 provider。';
    group.append(hint);
    const status = document.createElement('p');
    status.className = 'labtwin-voice-config-status';
    group.append(status);
    const save = document.createElement('button');
    save.type = 'button';
    save.textContent = '保存火山语音配置';
    group.append(save);
    form.insertBefore(group, legacyTts);

    save.addEventListener('click', () => {
      const payload = {};
      for (const name of ['volc_appkey', 'volc_token', 'volc_cluster', 'volc_speaker', 'volc_asr_cluster', 'volc_api_key']) {
        const value = String(form.elements.namedItem(name)?.value || '').trim();
        if (value) payload[name] = value;
      }
      save.disabled = true;
      status.classList.remove('form-error');
      status.textContent = '正在保存语音配置…';
      api('PUT', payload).then(() => {
        /* Keep the hidden React form state aligned so an unrelated AI save
         * cannot later restore stale AppID, speaker, or ASR Cluster values. */
        syncReactInput(legacyAsrInputs[0], payload.volc_appkey);
        syncReactInput(legacyAsrInputs[2], payload.volc_asr_cluster);
        syncReactInput(legacyTtsInputs[0], payload.volc_speaker);
        form.elements.namedItem('volc_token').value = '';
        form.elements.namedItem('volc_api_key').value = '';
        status.textContent = '语音配置已保存。新的 TTS/ASR 会话会读取最新配置。';
      }).catch(error => {
        status.textContent = `保存失败：${error.message}`;
        status.classList.add('form-error');
      }).finally(() => { save.disabled = false; });
    });
    load(form, status);
  }

  document.addEventListener('DOMContentLoaded', () => {
    const observer = new MutationObserver(() => mount());
    observer.observe(document.body, { childList: true, subtree: true });
    mount();
  });
})();
