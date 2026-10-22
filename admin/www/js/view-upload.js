// Upload view: pick a target album, drop files or whole folders, and upload
// them strictly one after another (the server holds each upload in memory).

import { get, post, uploadFile } from './api.js';
import { albumLabel, el, formatBytes, toast } from './ui.js';

const IMAGE_TYPES = new Set(['image/jpeg', 'image/png', 'image/webp']);
const IMAGE_EXTENSIONS = /\.(jpe?g|png|webp)$/i;
const NEW_ALBUM = '__new__';

const STATE_LABELS = {
  waiting: 'wartet',
  uploading: 'lädt hoch',
  done: 'fertig',
  error: 'Fehler',
  cancelled: 'abgebrochen',
};

const isImage = (file) => IMAGE_TYPES.has(file.type) || IMAGE_EXTENSIONS.test(file.name);

const joinAlbum = (base, sub) => [base, sub].filter(Boolean).join('/');

// Same rules as the server: no hidden or parent segments. Returns the cleaned
// name or throws with a message for the user.
export function cleanAlbumName(raw) {
  const segments = raw.replace(/\\/g, '/').split('/').map((part) => part.trim()).filter(Boolean);
  for (const segment of segments) {
    if (segment === '..' || segment.startsWith('.') || segment.startsWith('@')) {
      throw new Error(`„${segment}“ ist als Ordnername nicht erlaubt`);
    }
  }
  return segments.join('/');
}

// Files of a dropped folder, each with the sub folder path it came from
async function walkEntry(entry, path = '') {
  if (entry.isFile) {
    const file = await new Promise((resolve, reject) => entry.file(resolve, reject));
    return [{ file, folder: path }];
  }
  if (!entry.isDirectory) return [];
  const reader = entry.createReader();
  const children = [];
  // readEntries() returns the entries in batches until it returns none
  for (;;) {
    const batch = await new Promise((resolve, reject) => reader.readEntries(resolve, reject));
    if (!batch.length) break;
    children.push(...batch);
  }
  const folder = joinAlbum(path, entry.name);
  const nested = await Promise.all(children.map((child) => walkEntry(child, folder)));
  return nested.flat();
}

// Items of a drop event as [{ file, folder }]. Entries must be taken while the
// event is being handled, the items are gone afterwards.
async function droppedFiles(dataTransfer) {
  const entries = [...dataTransfer.items]
    .filter((item) => item.kind === 'file')
    .map((item) => item.webkitGetAsEntry?.())
    .filter(Boolean);
  if (!entries.length) return [...dataTransfer.files].map((file) => ({ file, folder: '' }));
  const nested = await Promise.all(entries.map((entry) => walkEntry(entry)));
  return nested.flat();
}

export default {
  title: 'Hochladen',

  async mount(root, { query }) {
    const queue = [];
    let running = false;
    let disposed = false;
    let dragDepth = 0;
    const uploadedAlbums = new Set();

    // Page structure

    const albumSelect = el('select', { id: 'up-album' });
    const newAlbumInput = el('input', { id: 'up-new-album', placeholder: 'z. B. reisen/norwegen', autocomplete: 'off' });
    const newAlbumField = el('label', { className: 'up-new-album', hidden: true }, ['Name des neuen Albums', newAlbumInput]);
    const fileInput = el('input', { type: 'file', multiple: true, accept: 'image/jpeg,image/png,image/webp', hidden: true });
    const dropZone = el('div', { className: 'up-drop', attrs: { role: 'button', tabindex: '0' } }, [
      el('strong', { textContent: 'Bilder oder Ordner hierher ziehen' }),
      el('span', { className: 'muted', textContent: 'JPEG, PNG oder WebP – Ordner werden zu Unteralben' }),
      el('button', { type: 'button', className: 'primary', textContent: 'Dateien auswählen', onclick: () => fileInput.click() }),
    ]);
    const skippedNote = el('p', { className: 'up-skipped muted small', hidden: true });
    const summary = el('p', { className: 'up-summary', attrs: { 'aria-live': 'polite' } });
    const list = el('ul', { className: 'up-list' });
    const retryButton = el('button', { type: 'button', className: 'small', textContent: 'Fehlgeschlagene erneut', hidden: true });
    const cancelButton = el('button', { type: 'button', className: 'small secondary', textContent: 'Wartende abbrechen', hidden: true });
    const clearButton = el('button', { type: 'button', className: 'small secondary', textContent: 'Erledigte entfernen', hidden: true });
    const doneHint = el('div', { className: 'up-done', hidden: true });
    const queueCard = el('section', { className: 'card up-queue', hidden: true }, [
      el('div', { className: 'row' }, [el('h2', { textContent: 'Warteschlange' }), el('span', { className: 'spacer' }), retryButton, cancelButton, clearButton]),
      summary,
      list,
      doneHint,
    ]);

    root.append(
      el('div', { className: 'page-header' }, [el('h1', { textContent: 'Hochladen' })]),
      el('div', { className: 'stack' }, [
        el('section', { className: 'card stack' }, [
          el('div', { className: 'form-grid two' }, [
            el('label', {}, ['Zielalbum', albumSelect]),
            newAlbumField,
          ]),
          dropZone,
          fileInput,
          skippedNote,
          el('p', { className: 'muted small', textContent: 'Bilder werden nacheinander hochgeladen. Wer die Seite verlässt, bricht die noch wartenden Uploads ab.' }),
        ]),
        queueCard,
      ]),
    );

    // Albums

    async function loadAlbums(selected) {
      let albums = [];
      try {
        albums = await get('/api/albums');
      } catch (error) {
        toast(`Alben nicht ladbar: ${error.message}`, 'error');
      }
      if (!albums.some((album) => album.name === '')) albums.unshift({ name: '', count: 0 });
      albumSelect.replaceChildren(
        ...albums.map((album) => el('option', { value: album.name, textContent: albumLabel(album.name) })),
        el('option', { value: NEW_ALBUM, textContent: 'Neues Album …' }),
      );
      albumSelect.value = albums.some((album) => album.name === selected) ? selected : '';
      updateNewAlbumField();
    }

    function updateNewAlbumField() {
      newAlbumField.hidden = albumSelect.value !== NEW_ALBUM;
      if (!newAlbumField.hidden) newAlbumInput.focus();
    }

    // The album new files go to; creates it first when "Neues Album" is chosen
    async function targetAlbum() {
      if (albumSelect.value !== NEW_ALBUM) return albumSelect.value;
      const name = cleanAlbumName(newAlbumInput.value);
      if (!name) throw new Error('Bitte einen Namen für das neue Album eingeben');
      const created = await post('/api/albums', { name });
      newAlbumInput.value = '';
      await loadAlbums(created.name);
      return created.name;
    }

    // Queue

    function renderItem(item) {
      const bar = el('span');
      const state = el('span', { className: 'up-state' });
      const remove = el('button', {
        type: 'button',
        className: 'small secondary',
        textContent: 'Entfernen',
        attrs: { 'aria-label': `${item.file.name} entfernen` },
        onclick: () => {
          item.status = 'cancelled';
          update(item);
        },
      });
      item.node = el('li', { className: 'up-item' }, [
        el('div', { className: 'up-item-head' }, [
          el('span', { className: 'up-name', textContent: item.file.name, title: item.file.name }),
          el('span', { className: 'muted small', textContent: `${formatBytes(item.file.size)} → ${albumLabel(item.album)}` }),
        ]),
        el('div', { className: 'progress' }, [bar]),
        el('div', { className: 'row' }, [state, el('span', { className: 'spacer' }), remove]),
      ]);
      Object.assign(item, { bar, state, remove });
      update(item);
      return item.node;
    }

    function update(item) {
      item.node.dataset.status = item.status;
      item.bar.style.width = `${Math.round((item.status === 'done' ? 1 : item.progress) * 100)}%`;
      let text = STATE_LABELS[item.status];
      if (item.status === 'uploading') text += ` ${Math.round(item.progress * 100)} %`;
      if (item.status === 'error') text += `: ${item.error}`;
      item.state.textContent = text;
      item.state.className = `up-state small ${item.status === 'error' ? 'error' : 'muted'}`;
      item.remove.hidden = item.status !== 'waiting';
      updateSummary();
    }

    function updateSummary() {
      const active = queue.filter((item) => item.status !== 'cancelled');
      const done = active.filter((item) => item.status === 'done').length;
      const failed = active.filter((item) => item.status === 'error').length;
      const waiting = active.filter((item) => item.status === 'waiting').length;
      summary.textContent = `${done} von ${active.length} hochgeladen` + (failed ? `, ${failed} fehlgeschlagen` : '');
      queueCard.hidden = queue.length === 0;
      retryButton.hidden = failed === 0 || running;
      cancelButton.hidden = waiting === 0;
      clearButton.hidden = !queue.some((item) => ['done', 'cancelled'].includes(item.status));
      updateDoneHint();
    }

    function updateDoneHint() {
      const finished = !running && uploadedAlbums.size > 0;
      doneHint.hidden = !finished;
      if (!finished) return;
      doneHint.replaceChildren(
        el('p', { className: 'muted small', textContent: 'Die Vorschaubilder und Bildvarianten entstehen jetzt im Hintergrund, das kann bei großen Bildern einen Moment dauern.' }),
        el('div', { className: 'row' }, [...uploadedAlbums].map((album) =>
          el('a', { className: 'button', href: `#/mediathek?album=${encodeURIComponent(album)}`, textContent: `In der Mediathek ansehen: ${albumLabel(album)}` }))),
      );
    }

    async function enqueue(entries) {
      const images = entries.filter(({ file }) => isImage(file));
      const skipped = entries.length - images.length;
      skippedNote.hidden = skipped === 0;
      skippedNote.textContent = skipped ? `${skipped} ${skipped === 1 ? 'Datei ist kein' : 'Dateien sind keine'} JPEG, PNG oder WebP und ${skipped === 1 ? 'wird' : 'werden'} übersprungen.` : '';
      if (!images.length) return;

      let album;
      try {
        album = await targetAlbum();
      } catch (error) {
        toast(error.message, 'error');
        return;
      }
      for (const { file, folder } of images) {
        const item = { file, album: joinAlbum(album, folder), status: 'waiting', progress: 0, error: '' };
        queue.push(item);
        list.append(renderItem(item));
      }
      run();
    }

    async function run() {
      if (running) return;
      running = true;
      window.addEventListener('beforeunload', warnBeforeLeaving);
      updateSummary();
      for (let item = nextWaiting(); item && !disposed; item = nextWaiting()) {
        item.status = 'uploading';
        update(item);
        try {
          await uploadFile(item.file, item.album, (progress) => {
            item.progress = progress;
            if (!disposed) update(item);
          });
          item.status = 'done';
          uploadedAlbums.add(item.album);
        } catch (error) {
          item.status = 'error';
          item.error = error.message;
          if (error.status === 401) break; // login screen takes over
        }
        if (!disposed) update(item);
      }
      running = false;
      window.removeEventListener('beforeunload', warnBeforeLeaving);
      if (!disposed) updateSummary();
    }

    const nextWaiting = () => queue.find((item) => item.status === 'waiting');

    function warnBeforeLeaving(event) {
      event.preventDefault();
      event.returnValue = '';
    }

    // Drag and drop anywhere on the page

    function hasFiles(event) {
      return [...(event.dataTransfer?.types || [])].includes('Files');
    }

    function onDragEnter(event) {
      if (!hasFiles(event)) return;
      dragDepth++;
      root.classList.add('up-dragging');
    }

    function onDragOver(event) {
      if (!hasFiles(event)) return;
      event.preventDefault();
      event.dataTransfer.dropEffect = 'copy';
    }

    function onDragLeave(event) {
      if (!hasFiles(event)) return;
      dragDepth = Math.max(0, dragDepth - 1);
      if (dragDepth === 0) root.classList.remove('up-dragging');
    }

    function onDrop(event) {
      if (!hasFiles(event)) return;
      event.preventDefault();
      dragDepth = 0;
      root.classList.remove('up-dragging');
      droppedFiles(event.dataTransfer)
        .then(enqueue)
        .catch((error) => toast(`Dateien nicht lesbar: ${error.message}`, 'error'));
    }

    // Wiring

    albumSelect.addEventListener('change', updateNewAlbumField);
    fileInput.addEventListener('change', () => {
      enqueue([...fileInput.files].map((file) => ({ file, folder: '' })));
      fileInput.value = '';
    });
    dropZone.addEventListener('keydown', (event) => {
      if (event.target === dropZone && (event.key === 'Enter' || event.key === ' ')) {
        event.preventDefault();
        fileInput.click();
      }
    });
    retryButton.addEventListener('click', () => {
      for (const item of queue.filter((entry) => entry.status === 'error')) {
        Object.assign(item, { status: 'waiting', progress: 0, error: '' });
        update(item);
      }
      run();
    });
    cancelButton.addEventListener('click', () => {
      for (const item of queue.filter((entry) => entry.status === 'waiting')) {
        item.status = 'cancelled';
        update(item);
      }
    });
    clearButton.addEventListener('click', () => {
      for (const item of queue.filter((entry) => ['done', 'cancelled'].includes(entry.status))) {
        item.node.remove();
        queue.splice(queue.indexOf(item), 1);
      }
      updateSummary();
    });

    const documentListeners = { dragenter: onDragEnter, dragover: onDragOver, dragleave: onDragLeave, drop: onDrop };
    for (const [type, listener] of Object.entries(documentListeners)) document.addEventListener(type, listener);

    await loadAlbums(query.get('album') ?? '');

    return () => {
      // Leaving the page drops what is still waiting; a running upload may finish
      disposed = true;
      for (const item of queue) if (item.status === 'waiting') item.status = 'cancelled';
      for (const [type, listener] of Object.entries(documentListeners)) document.removeEventListener(type, listener);
      window.removeEventListener('beforeunload', warnBeforeLeaving);
    };
  },
};
