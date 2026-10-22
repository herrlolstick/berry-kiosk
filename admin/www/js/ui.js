// Small DOM and formatting helpers shared by all views.

// el('button', { className: 'primary', onclick }, ['Text', child])
// Properties go onto the element, except "dataset" and "attrs" objects.
export function el(tag, properties = {}, children = []) {
  const node = document.createElement(tag);
  const { dataset, attrs, ...rest } = properties;
  Object.assign(node, rest);
  if (dataset) Object.assign(node.dataset, dataset);
  if (attrs) for (const [name, value] of Object.entries(attrs)) node.setAttribute(name, value);
  node.append(...[children].flat().filter((child) => child !== null && child !== undefined && child !== false));
  return node;
}

export function clear(node) {
  node.replaceChildren();
  return node;
}

// Short message at the bottom of the screen. kind: 'info' | 'error'
export function toast(message, kind = 'info') {
  let host = document.querySelector('#toasts');
  if (!host) {
    host = el('div', { id: 'toasts', attrs: { 'aria-live': 'polite' } });
    document.body.append(host);
  }
  const item = el('div', { className: `toast ${kind}`, textContent: message });
  host.append(item);
  setTimeout(() => item.remove(), kind === 'error' ? 6000 : 3000);
}

// Modal dialog with a body and buttons. Resolves with the value of the
// pressed button, or null when dismissed (Esc, backdrop).
// buttons: [{ label, value, kind: 'primary' | 'danger' | 'secondary' }]
export function dialog({ title, body, buttons = [{ label: 'OK', value: true, kind: 'primary' }], wide = false }) {
  return new Promise((resolve) => {
    const form = el('form', { method: 'dialog', className: 'dialog-body' }, [
      title ? el('h2', { textContent: title }) : null,
      body,
      el('div', { className: 'dialog-actions' },
        buttons.map((button) => el('button', {
          type: 'submit',
          value: String(button.value),
          className: button.kind || 'secondary',
          textContent: button.label,
        }))),
    ]);
    // Enter in a text field means "do it", not the first button (usually "Abbrechen")
    const primary = buttons.find((button) => button.kind === 'primary' || button.kind === 'danger');
    form.addEventListener('keydown', (event) => {
      if (event.key === 'Enter' && primary && event.target.matches('input:not([type=checkbox]):not([type=radio])')) {
        event.preventDefault();
        node.close(String(primary.value));
      }
    });
    const node = el('dialog', { className: wide ? 'wide' : '' }, [form]);
    node.addEventListener('close', () => {
      const pressed = buttons.find((button) => String(button.value) === node.returnValue);
      node.remove();
      resolve(pressed ? pressed.value : null);
    });
    node.addEventListener('click', (event) => {
      if (event.target === node) node.close();
    });
    document.body.append(node);
    node.showModal();
  });
}

export async function confirm(title, message, { label = 'OK', danger = false } = {}) {
  const result = await dialog({
    title,
    body: el('p', { textContent: message }),
    buttons: [
      { label: 'Abbrechen', value: false },
      { label, value: true, kind: danger ? 'danger' : 'primary' },
    ],
  });
  return result === true;
}

export function formatBytes(bytes) {
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

// "2024-06-12T05:31:10" -> "12. Juni 2024", with time if wanted
export function formatDate(value, withTime = false) {
  if (!value) return '';
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return '';
  const options = { day: 'numeric', month: 'long', year: 'numeric' };
  if (withTime) Object.assign(options, { hour: '2-digit', minute: '2-digit' });
  return date.toLocaleString('de-DE', options);
}

// "f/5,6 · 1/800 s · ISO 1600 · 400 mm" from a media object
export function formatExposure(media) {
  const number = (value) => value.toLocaleString('de-DE', { maximumFractionDigits: 1 });
  const parts = [];
  if (media.aperture) parts.push(`f/${number(media.aperture)}`);
  if (media.exposureTime) {
    parts.push(media.exposureTime < 0.25 ? `1/${Math.round(1 / media.exposureTime)} s` : `${number(media.exposureTime)} s`);
  }
  if (media.iso) parts.push(`ISO ${media.iso}`);
  if (media.focalLength) parts.push(`${number(media.focalLength)} mm`);
  return parts.join(' · ');
}

// "" is the root folder of the media library
export const albumLabel = (name) => (name ? name : 'Hauptordner');

export function debounce(fn, ms = 300) {
  let timer = null;
  return (...args) => {
    clearTimeout(timer);
    timer = setTimeout(() => fn(...args), ms);
  };
}

// Thumbnail <img> that falls back to a placeholder while the server has not
// rendered it yet
export function thumbnail(src, alt = '') {
  const img = el('img', { src, alt, loading: 'lazy', decoding: 'async' });
  img.addEventListener('error', () => {
    img.replaceWith(el('div', { className: 'thumb-placeholder', textContent: 'Vorschau folgt' }));
  });
  return img;
}
