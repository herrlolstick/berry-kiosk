// Detail view of one picture: large preview, facts and the edit form.
// view-library.js owns the list; this module only asks it for neighbours.

import { get, patch, del, imageUrl } from './api.js';
import { el, toast, confirm, formatBytes, formatDate, formatExposure, albumLabel } from './ui.js';

// The library worker needs a moment until an edit shows up in GET results
const REFRESH_DELAY_MS = 3000;
const SWIPE_DISTANCE = 50;

const FIELDS = [
  { key: 'title', label: 'Titel' },
  { key: 'location', label: 'Ort' },
  { key: 'description', label: 'Beschreibung', multiline: true },
  { key: 'quote', label: 'Zitat', multiline: true },
  { key: 'quoteAuthor', label: 'Zitat von' },
];

const fileName = (media) => media.path.split('/').pop();

function isTyping(target) {
  return target instanceof HTMLElement && (target.isContentEditable || ['INPUT', 'TEXTAREA', 'SELECT'].includes(target.tagName));
}

// options: { id, media (or null), nav: { position(), step(direction) },
//            open(id), close(), changed(media), deleted(id) -> next id | null }
export function mountPhoto(root, { id, media: known, nav, open, close, changed, deleted }) {
  let media = known;
  let alive = true;
  let refreshTimer = null;
  let inputs = {};
  let enabledBox = null;
  let saveButton = null;

  const image = el('img', { className: 'lib-preview', alt: '' });
  const stageMessage = el('p', { className: 'lib-stage-message', hidden: true });
  const prev = el('button', {
    type: 'button', className: 'lib-step prev', textContent: '‹', title: 'Vorheriges Bild (←)', attrs: { 'aria-label': 'Vorheriges Bild' },
  });
  const next = el('button', {
    type: 'button', className: 'lib-step next', textContent: '›', title: 'Nächstes Bild (→)', attrs: { 'aria-label': 'Nächstes Bild' },
  });
  const closeButton = el('button', {
    type: 'button', className: 'lib-close', textContent: '✕', title: 'Zur Übersicht (Esc)', attrs: { 'aria-label': 'Zur Übersicht' },
  });
  const counter = el('span', { className: 'lib-counter' });
  const stage = el('div', { className: 'lib-stage' }, [image, stageMessage, prev, next, closeButton, counter]);
  const panel = el('aside', { className: 'lib-panel card' }, [el('p', { className: 'muted', textContent: 'Lädt …' })]);
  root.append(el('div', { className: 'lib-photo' }, [stage, panel]));

  // Preview: the 1080p rendition, else whatever is best (2160p or the original)

  function showImage() {
    let fellBack = false;
    stageMessage.hidden = true;
    image.hidden = false;
    image.alt = media.title || fileName(media);
    image.onerror = () => {
      if (!fellBack) {
        fellBack = true;
        image.src = imageUrl(media);
      } else {
        image.hidden = true;
        stageMessage.textContent = 'Vorschau nicht verfügbar';
        stageMessage.hidden = false;
      }
    };
    image.src = imageUrl(media, '1080p');
  }

  function updateNav() {
    const position = nav.position();
    prev.disabled = !position.hasPrev;
    next.disabled = !position.hasNext;
    counter.textContent = position.index >= 0 ? `${position.index + 1} / ${position.total}` : '';
  }

  // Panel

  function facts() {
    const rows = [
      ['Album', albumLabel(media.album)],
      ['Datei', media.path],
      ['Aufnahme', formatDate(media.taken, true)],
      ['Größe', media.width && media.height ? `${media.width} × ${media.height} px` : ''],
      ['Dateigröße', media.fileSize ? formatBytes(media.fileSize) : ''],
      ['Kamera', media.camera],
      ['Objektiv', media.lens],
      ['Belichtung', formatExposure(media)],
      ['Varianten', media.renditions?.length ? media.renditions.join(', ') : 'noch keine'],
      ['Hinzugefügt', formatDate(media.addedAt, true)],
    ];
    return el('dl', { className: 'lib-facts' },
      rows.filter(([, value]) => value).flatMap(([label, value]) => [el('dt', { textContent: label }), el('dd', { textContent: value })]));
  }

  function form() {
    inputs = {};
    const fields = FIELDS.map((field) => {
      const input = field.multiline
        ? el('textarea', { rows: 3, value: media[field.key] ?? '' })
        : el('input', { value: media[field.key] ?? '' });
      inputs[field.key] = input;
      return el('label', { className: field.multiline ? 'full' : '' }, [field.label, input]);
    });
    enabledBox = el('input', { type: 'checkbox', checked: media.enabled !== false });
    saveButton = el('button', { type: 'submit', className: 'primary', textContent: 'Speichern' });
    const removeButton = el('button', { type: 'button', className: 'danger secondary lib-remove', textContent: 'Löschen' });
    removeButton.addEventListener('click', remove);

    const node = el('form', { className: 'form-grid lib-edit' }, [
      ...fields,
      el('label', { className: 'inline full' }, [enabledBox, 'In der Diashow zeigen']),
      el('div', { className: 'row full' }, [saveButton, el('span', { className: 'spacer' }), removeButton]),
    ]);
    node.addEventListener('submit', save);
    return node;
  }

  function renderPanel() {
    const hidden = el('p', { hidden: media.enabled !== false }, [el('span', { className: 'badge warning', textContent: 'ausgeblendet' })]);
    panel.replaceChildren(
      el('h1', { className: 'lib-title', textContent: media.title || fileName(media) }),
      hidden,
      facts(),
      el('p', {}, [el('a', { href: imageUrl(media, 'original'), target: '_blank', rel: 'noopener', textContent: 'Original öffnen ↗' })]),
      el('h2', { textContent: 'Bearbeiten' }),
      form(),
    );
  }

  function render() {
    showImage();
    renderPanel();
    updateNav();
  }

  // Editing

  function changes() {
    const result = {};
    for (const field of FIELDS) {
      const value = inputs[field.key].value.trim();
      if (value !== (media[field.key] ?? '')) result[field.key] = value === '' ? null : value;
    }
    if (enabledBox.checked !== (media.enabled !== false)) result.enabled = enabledBox.checked;
    return result;
  }

  const isDirty = () => Boolean(enabledBox) && Object.keys(changes()).length > 0;

  function scheduleRefresh() {
    clearTimeout(refreshTimer);
    refreshTimer = setTimeout(async () => {
      try {
        const fresh = await get(`/api/media/${media.id}`);
        if (!alive || isDirty()) return;
        media = fresh;
        changed(fresh);
        render();
      } catch {
        // Not important enough to bother anyone; the next visit loads it again
      }
    }, REFRESH_DELAY_MS);
  }

  async function save(event) {
    event.preventDefault();
    const update = changes();
    if (!Object.keys(update).length) {
      toast('Keine Änderungen');
      return;
    }
    saveButton.disabled = true;
    try {
      await patch(`/api/media/${media.id}`, update);
      if (!alive) return;
      for (const [key, value] of Object.entries(update)) {
        if (value === null) delete media[key];
        else media[key] = value;
      }
      changed(media);
      renderPanel();
      toast('Gespeichert');
      scheduleRefresh();
    } catch (error) {
      toast(error.message, 'error');
      if (saveButton) saveButton.disabled = false;
    }
  }

  async function remove() {
    const ok = await confirm(
      'Bild löschen?',
      `„${media.title || fileName(media)}“ wird in den Papierkorb des Kiosks verschoben und verschwindet aus der Diashow.`,
      { label: 'Löschen', danger: true },
    );
    if (!ok) return;
    try {
      await del(`/api/media/${media.id}`);
      toast('In den Papierkorb verschoben');
      const nextId = deleted(media.id);
      if (nextId) open(nextId);
      else close();
    } catch (error) {
      toast(error.message, 'error');
    }
  }

  // Leaving: never lose typed changes without asking

  async function leave(action) {
    if (isDirty()) {
      const discard = await confirm('Änderungen verwerfen?', 'Die Änderungen an diesem Bild sind noch nicht gespeichert.', {
        label: 'Verwerfen', danger: true,
      });
      if (!discard) return;
    }
    action();
  }

  async function step(direction) {
    const position = nav.position();
    if ((direction < 0 && !position.hasPrev) || (direction > 0 && !position.hasNext)) return;
    await leave(async () => {
      try {
        const target = await nav.step(direction);
        if (target && alive) open(target);
        else updateNav();
      } catch (error) {
        toast(error.message, 'error');
      }
    });
  }

  prev.addEventListener('click', () => step(-1));
  next.addEventListener('click', () => step(1));
  closeButton.addEventListener('click', () => leave(close));

  const onKey = (event) => {
    if (document.querySelector('dialog[open]') || event.altKey || event.metaKey || event.ctrlKey) return;
    if (event.key === 'Escape') {
      if (isTyping(event.target)) event.target.blur();
      else leave(close);
      return;
    }
    if (isTyping(event.target)) return;
    if (event.key === 'ArrowLeft') step(-1);
    if (event.key === 'ArrowRight') step(1);
  };
  document.addEventListener('keydown', onKey);

  // Swipe on phones
  let swipeStart = null;
  stage.addEventListener('pointerdown', (event) => {
    if (event.pointerType !== 'mouse') swipeStart = event.clientX;
  });
  stage.addEventListener('pointerup', (event) => {
    if (swipeStart === null) return;
    const distance = event.clientX - swipeStart;
    swipeStart = null;
    if (Math.abs(distance) >= SWIPE_DISTANCE) step(distance < 0 ? 1 : -1);
  });
  stage.addEventListener('pointercancel', () => { swipeStart = null; });

  // Start

  if (media) {
    render();
  } else {
    updateNav();
    get(`/api/media/${id}`)
      .then((fresh) => {
        if (!alive) return;
        media = fresh;
        render();
      })
      .catch((error) => {
        if (!alive) return;
        image.hidden = true;
        stageMessage.textContent = error.status === 404 ? 'Dieses Bild gibt es nicht (mehr).' : error.message;
        stageMessage.hidden = false;
        const back = el('button', { type: 'button', textContent: 'Zur Übersicht' });
        back.addEventListener('click', close);
        panel.replaceChildren(el('p', { className: 'error', textContent: stageMessage.textContent }), back);
      });
  }

  return () => {
    alive = false;
    clearTimeout(refreshTimer);
    document.removeEventListener('keydown', onKey);
  };
}
