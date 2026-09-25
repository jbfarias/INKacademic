const statusElement = document.getElementById('firmware-status');
const fileElement = document.getElementById('firmware-file');
const signatureElement = document.getElementById('firmware-signature-file');
const modeElement = document.getElementById('firmware-mode');
const signatureGroup = document.getElementById('firmware-signature-group');
const urlElement = document.getElementById('firmware-url');
const manualDownloadButton = document.getElementById('firmware-manual-download-btn');
const sourceSaveButton = document.getElementById('firmware-source-save-btn');
const sourceOfficialButton = document.getElementById('firmware-source-official-btn');
const sourceStatus = document.getElementById('firmware-source-status');
const uploadButton = document.getElementById('firmware-upload-btn');
const installButton = document.getElementById('firmware-install-btn');
const cancelButton = document.getElementById('firmware-cancel-btn');
const checkButton = document.getElementById('firmware-check-btn');
const downloadButton = document.getElementById('firmware-download-btn');
const officialStatus = document.getElementById('firmware-official-status');
const progressWrap = document.getElementById('firmware-progress-wrap');
const progressBar = document.getElementById('firmware-progress-bar');
const progressText = document.getElementById('firmware-progress-text');

let currentStatus = null;
let uploadInProgress = false;
const firmwareHeaders = { 'X-Inkademic-Firmware': '1' };

function setStatus(message, isError = false) {
  statusElement.textContent = message;
  statusElement.style.color = isError ? '#b91c1c' : '';
}

function describeStatus(data) {
  const state = data.state || 'unknown';
  const device = data.device || 'device';
  const version = data.version || 'unknown version';
  if (data.error) return `${device} — ${state}\n${data.error}`;
  if (state === 'ready') return data.mode === 'manual'
    ? `${device} — ${version}\nManual image ready (${data.size || 0} bytes). Integrity checked; origin not verified by INKademic.`
    : `${device} — ${version}\nSigned image ${data.candidateVersion || ''} validated and ready to install (${data.size || 0} bytes).`;
  if (state === 'awaiting_signature') return `${device}\nImage received (${data.received || 0} bytes). Select its 64-byte Ed25519 signature.`;
  if (state === 'install_requested') return 'Installation queued. The device will reboot shortly.';
  if (state === 'installing') return `Installing ${device}… ${data.received || 0} / ${data.total || data.size || 0} bytes`;
  if (state === 'uploading') return `Uploading ${device}… ${data.received || 0} / ${data.total || 0} bytes`;
  if (state === 'rebooting') return 'Firmware written successfully. Waiting for the device to reboot…';
  if (state === 'completed') return `${device} — ${version}\nThe last browser firmware update completed.`;
  if (state === 'interrupted') return `${device}\nThe previous installation was interrupted. The active slot was retained.`;
  if (state === 'failed') return `${device}\nFirmware update failed.`;
  return `${device} — ${version}\nNo staged firmware image.`;
}

function renderStatus(data) {
  currentStatus = data;
  setStatus(describeStatus(data), Boolean(data.error));
  const state = data.state || 'idle';
  const ready = state === 'ready';
  const busy = uploadInProgress || ['install_requested', 'installing', 'rebooting'].includes(state);
  installButton.disabled = !ready || busy;
  cancelButton.disabled = busy || (!ready && !['failed', 'interrupted', 'awaiting_signature', 'uploading'].includes(state));
  uploadButton.disabled = busy;
  modeElement.disabled = busy;
  manualDownloadButton.disabled = busy;
  if ((state === 'installing' || state === 'uploading') && (data.total || data.size)) {
    progressWrap.hidden = false;
    const percent = Math.min(100, Math.round(((data.received || 0) * 100) / (data.total || data.size)));
    progressBar.style.width = `${percent}%`;
    progressText.textContent = `${percent}%`;
  }
}

async function refreshStatus() {
  try {
    const response = await fetch('/api/firmware/status?_=' + Date.now());
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    renderStatus(await response.json());
  } catch (error) {
    setStatus(`Device is restarting or unavailable: ${error.message}`, true);
  }
}

function uploadChunk(file, session, offset, mode) {
  return new Promise((resolve, reject) => {
    const chunkSize = 64 * 1024;
    const chunk = file.slice(offset, Math.min(offset + chunkSize, file.size));
    const form = new FormData();
    form.append('file', chunk, file.name);
    const xhr = new XMLHttpRequest();
    xhr.open('POST', `/api/firmware/upload?session=${encodeURIComponent(session)}&offset=${offset}&total=${file.size}&mode=${mode}`, true);
    xhr.setRequestHeader('X-Inkademic-Firmware', '1');
    xhr.upload.onprogress = (event) => {
      if (!event.lengthComputable) return;
      progressWrap.hidden = false;
      const percent = Math.round(((offset + event.loaded) * 100) / file.size);
      progressBar.style.width = `${percent}%`;
      progressText.textContent = `Uploading — ${percent}%`;
    };
    xhr.onload = () => {
      let data;
      try { data = JSON.parse(xhr.responseText); } catch (_) { data = { error: xhr.responseText }; }
      if (xhr.status >= 200 && xhr.status < 300) resolve(data);
      else reject(new Error(data.error || `HTTP ${xhr.status}`));
    };
    xhr.onerror = () => reject(new Error('Network error during firmware upload'));
    xhr.onabort = () => reject(new Error('Firmware upload cancelled'));
    xhr.send(form);
  });
}

async function uploadFirmware(file, mode) {
  const existing = currentStatus && currentStatus.filename === file.name && currentStatus.total === file.size &&
    currentStatus.session && currentStatus.mode === mode && ['uploading', 'awaiting_signature'].includes(currentStatus.state);
  const session = existing ? currentStatus.session :
    (window.crypto && window.crypto.randomUUID ? window.crypto.randomUUID() : `${Date.now()}-${Math.random()}`);
  let offset = existing ? Number(currentStatus.received || 0) : 0;
  while (offset < file.size) {
    const data = await uploadChunk(file, session, offset, mode);
    offset = Number(data.received || Math.min(offset + 64 * 1024, file.size));
    currentStatus = { ...currentStatus, ...data, state: data.state || 'uploading', session, mode, filename: file.name, total: file.size, received: offset };
  }
  return await (await fetch('/api/firmware/status?_=' + Date.now())).json();
}

function uploadSignature(file) {
  return new Promise((resolve, reject) => {
    const form = new FormData();
    form.append('signature', file, file.name);
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/firmware/signature', true);
    xhr.setRequestHeader('X-Inkademic-Firmware', '1');
    xhr.onload = () => {
      let data;
      try { data = JSON.parse(xhr.responseText); } catch (_) { data = { error: xhr.responseText }; }
      if (xhr.status >= 200 && xhr.status < 300) resolve(data);
      else reject(new Error(data.error || `HTTP ${xhr.status}`));
    };
    xhr.onerror = () => reject(new Error('Network error during signature upload'));
    xhr.send(form);
  });
}

uploadButton.addEventListener('click', async () => {
  const file = fileElement.files[0];
  const signature = signatureElement.files[0];
  const mode = modeElement.value;
  if (!file) return setStatus('Choose a firmware .bin file first.', true);
  if (!file.name.toLowerCase().endsWith('.bin')) return setStatus('The firmware file must end in .bin.', true);
  if (mode === 'signed' && !signature) return setStatus('Choose the matching Ed25519 .sig file.', true);
  if (mode === 'signed' && signature.size !== 64) return setStatus('The Ed25519 signature must be exactly 64 bytes.', true);
  uploadInProgress = true;
  uploadButton.disabled = true;
  modeElement.disabled = true;
  try {
    renderStatus(await uploadFirmware(file, mode));
    if (mode === 'signed' && currentStatus.state === 'awaiting_signature') {
      renderStatus(await uploadSignature(signature));
    }
    await refreshStatus();
  } catch (error) {
    setStatus(error.message, true);
  } finally {
    uploadInProgress = false;
    uploadButton.disabled = false;
    modeElement.disabled = false;
  }
});

checkButton.addEventListener('click', async () => {
  checkButton.disabled = true;
  try {
    const response = await fetch('/api/firmware/catalog?_=' + Date.now(), { headers: firmwareHeaders });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
    officialStatus.textContent = data.available
      ? `Official release ${data.version} is available (${data.size} bytes, signed Ed25519).`
      : `No newer signed release than ${data.currentVersion} is available.`;
    downloadButton.disabled = !data.available;
  } catch (error) {
    officialStatus.textContent = error.message;
    downloadButton.disabled = true;
  } finally {
    checkButton.disabled = false;
  }
});

downloadButton.addEventListener('click', async () => {
  if (!window.confirm('Download the official signed release to the device?')) return;
  downloadButton.disabled = true;
  try {
    const response = await fetch('/api/firmware/download', { method: 'POST', headers: firmwareHeaders });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
    renderStatus(data);
  } catch (error) {
    setStatus(error.message, true);
    downloadButton.disabled = false;
  }
});

installButton.addEventListener('click', async () => {
  if (!currentStatus || currentStatus.state !== 'ready') return;
  const confirmation = currentStatus.mode === 'manual'
    ? 'Install this manually selected firmware and reboot? Its origin has not been verified by INKademic.'
    : 'Install the signed firmware and reboot the device now?';
  if (!window.confirm(confirmation)) return;
  installButton.disabled = true;
  try {
    renderStatus(await firmwareAction('/api/firmware/install', {
      mode: currentStatus.mode, sha256: currentStatus.candidateSha256
    }));
    setStatus('Installation queued. Keep the device powered while it reboots.');
  } catch (error) {
    setStatus(error.message, true);
  }
});

cancelButton.addEventListener('click', async () => {
  if (!window.confirm('Discard the staged firmware image?')) return;
  try {
    renderStatus(await firmwareAction('/api/firmware/cancel'));
    progressWrap.hidden = true;
  } catch (error) {
    setStatus(error.message, true);
  }
});

async function firmwareAction(path, values) {
  const response = await fetch(path, {
    method: 'POST', headers: firmwareHeaders,
    ...(values ? { body: new URLSearchParams(values) } : {})
  });
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
  return data;
}

modeElement.addEventListener('change', () => {
  signatureGroup.hidden = modeElement.value === 'manual';
});

function manualUrl() {
  const value = urlElement.value.trim();
  const url = new URL(value);
  if (!['http:', 'https:'].includes(url.protocol) || !url.hostname || url.username || url.password || value.length > 1024)
    throw new Error('Enter a direct HTTP(S) firmware URL.');
  return value;
}

manualDownloadButton.addEventListener('click', async () => {
  manualDownloadButton.disabled = true;
  try {
    const url = manualUrl();
    renderStatus(await firmwareAction('/api/firmware/manual-download', { mode: 'manual', url }));
  } catch (error) {
    setStatus(error.message, true);
  } finally {
    manualDownloadButton.disabled = false;
  }
});

function describeSource(data) {
  sourceStatus.textContent = data.mode === 'manual'
    ? `Device OTA menu: manual source — ${data.url}`
    : 'Device OTA menu: official signed INKademic releases.';
}

sourceSaveButton.addEventListener('click', async () => {
  try { describeSource(await firmwareAction('/api/firmware/source', { mode: 'manual', url: manualUrl() })); }
  catch (error) { sourceStatus.textContent = error.message; }
});
sourceOfficialButton.addEventListener('click', async () => {
  try { describeSource(await firmwareAction('/api/firmware/source', { mode: 'official' })); }
  catch (error) { sourceStatus.textContent = error.message; }
});
async function refreshSource() {
  try {
    const response = await fetch('/api/firmware/source', { headers: firmwareHeaders });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
    if (data.url) urlElement.value = data.url;
    describeSource(data);
  } catch (error) { sourceStatus.textContent = error.message; }
}

refreshSource();
refreshStatus();
setInterval(refreshStatus, 2000);
