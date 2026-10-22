'use strict';

// Test console for the berry-kiosk admin API (milestone 4). The full UI follows in milestone 5.

const $ = (selector) => document.querySelector(selector);
let statusTimer = null;
let screenshotTimer = null;

class ApiError extends Error {
  constructor(status, message, body) {
    super(message);
    this.status = status;
    this.body = body;
  }
}

async function api(method, path, body) {
  const options = { method, headers: {} };
  if (method !== 'GET') options.headers['X-Berry-Kiosk'] = '1';
  if (body !== undefined) {
    options.headers['Content-Type'] = 'application/json';
    options.body = JSON.stringify(body);
  }
  const response = await fetch(path, options);
  const data = response.status === 204 ? null : await response.json().catch(() => null);
  if (!response.ok) {
    if (response.status === 401 && !path.startsWith('/api/auth/')) showLogin();
    throw new ApiError(response.status, data?.error || `HTTP ${response.status}`, data);
  }
  return data;
}

function element(tag, properties = {}, children = []) {
  const node = Object.assign(document.createElement(tag), properties);
  node.append(...children);
  return node;
}

function showError(target, error) {
  target.textContent = error ? error.message : '';
  target.hidden = !error;
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes)) return '–';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  return `${value.toLocaleString('de-DE', { maximumFractionDigits: 1 })} ${units[unit]}`;
}

function thumbnail(id, alt) {
  const img = element('img', { alt, loading: 'lazy', src: `/api/media/${id}/thumb` });
  // Not rendered yet: show a placeholder instead of a broken image
  img.addEventListener('error', () => img.replaceWith(element('div', { className: 'placeholder', textContent: 'noch keine Vorschau' })));
  return img;
}

// Login

function showLogin() {
  clearInterval(statusTimer);
  clearInterval(screenshotTimer);
  $('#app').hidden = true;
  $('#login').hidden = false;
  $('#login-form').password.focus();
}

async function showApp(username) {
  $('#login').hidden = true;
  $('#app').hidden = false;
  $('#user').textContent = username;
  await Promise.allSettled([refreshStatus(), refreshScreenshot(), loadAlbums(), loadMedia()]);
  statusTimer = setInterval(refreshStatus, 10000);
  screenshotTimer = setInterval(() => { if (!document.hidden) refreshScreenshot(); }, 15000);
}

$('#login-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const form = event.target;
  try {
    const result = await api('POST', '/api/auth/login', { username: form.username.value, password: form.password.value });
    form.password.value = '';
    showError($('#login-error'), null);
    await showApp(result.username);
  } catch (error) {
    if (error.status === 429 && error.body?.retryAfter)
      error.message = `${error.message} Bitte in ${Math.ceil(error.body.retryAfter)} s erneut versuchen.`;
    showError($('#login-error'), error);
  }
});

$('#logout').addEventListener('click', async () => {
  await api('POST', '/api/auth/logout').catch(() => {});
  showLogin();
});

// Display

async function refreshStatus() {
  const status = await api('GET', '/api/status').catch(() => null);
  if (!status) return;
  const display = status.display || {};
  const parts = [display.connected ? 'verbunden' : 'getrennt'];
  if (display.renderTier) parts.push(`Render-Stufe ${display.renderTier}`);
  if (display.layout) parts.push(`Layout „${display.layout}“`);
  if (display.paused) parts.push('pausiert');
  if (display.device) parts.push(display.device);
  if (display.screenSize) parts.push(display.screenSize.join(' × '));
  $('#display-status').textContent = parts.join(' · ');

  const showing = (display.showing || []).filter((item) => item.id);
  $('#showing').replaceChildren(...showing.map((item) =>
    element('figure', {}, [thumbnail(item.id, item.path), element('figcaption', { textContent: item.region })])));

  const library = status.library || {};
  const storage = status.storage || {};
  $('#library-status').textContent = [
    `${library.images ?? '–'} Bilder`,
    `${library.pendingRenditions ?? '–'} offene Varianten`,
    `${formatBytes(storage.free)} frei von ${formatBytes(storage.total)}`,
    `Version ${status.version ?? '–'}`,
  ].join(' · ');
}

async function refreshScreenshot() {
  const img = $('#screenshot');
  const url = `/api/display/screenshot?maxWidth=1280&t=${Date.now()}`;
  // Load off-screen first so a failed request keeps the last picture
  const probe = new Image();
  probe.onload = () => { img.src = probe.src; showError($('#display-error'), null); };
  probe.onerror = () => showError($('#display-error'), new Error('Kein Bildschirmfoto – ist die Anzeige verbunden?'));
  probe.src = url;
}

document.querySelectorAll('[data-display]').forEach((button) => {
  button.addEventListener('click', async () => {
    try {
      await api('POST', `/api/display/${button.dataset.display}`);
      showError($('#display-error'), null);
      setTimeout(() => { refreshStatus(); refreshScreenshot(); }, 1500);
    } catch (error) {
      showError($('#display-error'), error);
    }
  });
});

$('#screenshot-refresh').addEventListener('click', refreshScreenshot);

$('#sync').addEventListener('click', async () => {
  await api('POST', '/api/library/sync').catch(() => {});
  setTimeout(refreshStatus, 2000);
});

// Upload

async function loadAlbums(selected) {
  const albums = await api('GET', '/api/albums').catch(() => []);
  const select = $('#album');
  const current = selected ?? select.value;
  if (!albums.some((album) => album.name === '')) albums.unshift({ name: '', count: 0 });
  select.replaceChildren(...albums.map((album) =>
    element('option', { value: album.name, textContent: `${album.name || '(Hauptordner)'} (${album.count})` })));
  select.value = albums.some((album) => album.name === current) ? current : '';
}

$('#create-album').addEventListener('click', async () => {
  const input = $('#new-album');
  if (!input.value.trim()) return;
  try {
    const album = await api('POST', '/api/albums', { name: input.value.trim() });
    input.value = '';
    await loadAlbums(album.name);
  } catch (error) {
    alert(error.message);
  }
});

function uploadFile(file, album, progress) {
  return new Promise((resolve, reject) => {
    const query = new URLSearchParams({ album, name: file.name });
    const request = new XMLHttpRequest();
    request.open('POST', `/api/media?${query}`);
    request.setRequestHeader('X-Berry-Kiosk', '1');
    request.setRequestHeader('Content-Type', file.type || 'application/octet-stream');
    request.upload.onprogress = (event) => { if (event.lengthComputable) progress.value = event.loaded / event.total; };
    request.onload = () => {
      let data = null;
      try { data = JSON.parse(request.responseText); } catch { /* empty body */ }
      if (request.status === 401) showLogin();
      if (request.status >= 200 && request.status < 300) resolve(data);
      else reject(new Error(data?.error || `HTTP ${request.status}`));
    };
    request.onerror = () => reject(new Error('Netzwerkfehler'));
    request.send(file);
  });
}

async function uploadFiles(files) {
  const album = $('#album').value;
  for (const file of files) {
    const progress = element('progress', { max: 1, value: 0 });
    const label = element('span', { textContent: file.name });
    const row = element('li', {}, [label, progress]);
    $('#uploads').prepend(row);
    try {
      const result = await uploadFile(file, album, progress);
      progress.replaceWith(element('span', { className: 'done', textContent: 'hochgeladen' }));
      label.textContent = result?.path || file.name;
    } catch (error) {
      progress.replaceWith(element('span', { className: 'error', textContent: error.message }));
    }
  }
  await Promise.allSettled([loadAlbums(), loadMedia(), refreshStatus()]);
}

$('#files').addEventListener('change', (event) => {
  uploadFiles([...event.target.files]);
  event.target.value = '';
});

const dropzone = $('#dropzone');
dropzone.addEventListener('dragover', (event) => { event.preventDefault(); dropzone.classList.add('over'); });
dropzone.addEventListener('dragleave', () => dropzone.classList.remove('over'));
dropzone.addEventListener('drop', (event) => {
  event.preventDefault();
  dropzone.classList.remove('over');
  uploadFiles([...event.dataTransfer.files].filter((file) => file.type.startsWith('image/')));
});

// Media

function mediaTile(media) {
  const toggle = element('button', { className: 'secondary small', textContent: media.enabled ? 'Ausblenden' : 'Einblenden' });
  toggle.addEventListener('click', () => changeMedia(() => api('PATCH', `/api/media/${media.id}`, { enabled: !media.enabled })));
  const remove = element('button', { className: 'secondary small', textContent: 'Löschen' });
  remove.addEventListener('click', () => {
    if (confirm(`„${media.title || media.path}“ in den Papierkorb verschieben?`))
      changeMedia(() => api('DELETE', `/api/media/${media.id}`));
  });
  return element('div', { className: media.enabled ? 'tile' : 'tile disabled' }, [
    thumbnail(media.id, media.title || media.path),
    element('span', { className: 'name', textContent: media.title || media.path, title: media.path }),
    element('div', { className: 'buttons' }, [toggle, remove]),
  ]);
}

async function changeMedia(action) {
  try {
    await action();
    showError($('#media-error'), null);
    // The server syncs in the background; give it a moment before reloading
    setTimeout(loadMedia, 1500);
  } catch (error) {
    showError($('#media-error'), error);
  }
}

async function loadMedia() {
  try {
    const result = await api('GET', '/api/media?limit=24');
    $('#media').replaceChildren(...result.items.map(mediaTile));
    if (!result.items.length) $('#media').append(element('p', { className: 'muted', textContent: 'Noch keine Bilder.' }));
    showError($('#media-error'), null);
  } catch (error) {
    showError($('#media-error'), error);
  }
}

$('#reload-media').addEventListener('click', loadMedia);

// Start

(async () => {
  try {
    const session = await api('GET', '/api/auth/session');
    if (session.authenticated) await showApp(session.username);
    else showLogin();
  } catch {
    showLogin();
  }
})();
