// Client for the berry-kiosk admin API (see README, "Admin web API").
// Every request that changes something carries the CSRF header; a 401
// anywhere sends the app back to the login screen.

export class ApiError extends Error {
  constructor(status, message, body) {
    super(message);
    this.status = status;
    this.body = body;
  }
}

const listeners = new Set();

// Called with no arguments whenever the server says the session is gone
export function onUnauthorized(listener) {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

function unauthorized(path) {
  if (!path.startsWith('/api/auth/')) listeners.forEach((listener) => listener());
}

async function parse(response) {
  if (response.status === 204) return null;
  const type = response.headers.get('Content-Type') || '';
  return type.includes('application/json') ? response.json().catch(() => null) : null;
}

export async function api(method, path, body) {
  const options = { method, headers: {} };
  if (method !== 'GET') options.headers['X-Berry-Kiosk'] = '1';
  if (body !== undefined) {
    options.headers['Content-Type'] = 'application/json';
    options.body = JSON.stringify(body);
  }
  const response = await fetch(path, options);
  const data = await parse(response);
  if (!response.ok) {
    if (response.status === 401) unauthorized(path);
    throw new ApiError(response.status, data?.error || `HTTP ${response.status}`, data);
  }
  return data;
}

export const get = (path) => api('GET', path);
export const post = (path, body) => api('POST', path, body);
export const patch = (path, body) => api('PATCH', path, body);
export const put = (path, body) => api('PUT', path, body);
export const del = (path) => api('DELETE', path);

// Builds "/api/media?album=x&q=y", leaving out empty values
export function withQuery(path, params = {}) {
  const query = new URLSearchParams();
  for (const [key, value] of Object.entries(params)) {
    if (value !== undefined && value !== null && value !== '') query.set(key, value);
  }
  const text = query.toString();
  return text ? `${path}?${text}` : path;
}

// Uploads one file into an album. onProgress receives 0…1.
// Resolves with { path, album }, rejects with ApiError.
export function uploadFile(file, album, onProgress) {
  return new Promise((resolve, reject) => {
    const request = new XMLHttpRequest();
    request.open('POST', withQuery('/api/media', { album, name: file.name }));
    request.setRequestHeader('X-Berry-Kiosk', '1');
    request.setRequestHeader('Content-Type', file.type || 'application/octet-stream');
    request.upload.addEventListener('progress', (event) => {
      if (event.lengthComputable && onProgress) onProgress(event.loaded / event.total);
    });
    request.addEventListener('load', () => {
      let data = null;
      try { data = JSON.parse(request.responseText); } catch { /* not JSON */ }
      if (request.status >= 200 && request.status < 300) {
        resolve(data);
      } else {
        if (request.status === 401) unauthorized('/api/media');
        reject(new ApiError(request.status, data?.error || `HTTP ${request.status}`, data));
      }
    });
    request.addEventListener('error', () => reject(new ApiError(0, 'Netzwerkfehler')));
    request.send(file);
  });
}

// URLs of a picture's files. version changes when the file changes, so
// browsers do not keep showing an old thumbnail.
export const thumbUrl = (media) => `/api/media/${media.id}/thumb?v=${media.version ?? 0}`;
export const imageUrl = (media, kind = '') =>
  withQuery(`/api/media/${media.id}/image`, { kind, v: media.version ?? 0 });
