// Media library: filterable grid with infinite scroll and bulk actions,
// plus the detail view of one picture (library-photo.js).
//
//   #/mediathek?album=…&q=…&status=…&sort=…   grid
//   #/mediathek/<id>?<same filters>            one picture

import { get, post, withQuery, thumbUrl } from './api.js';
import { el, clear, toast, dialog, confirm, debounce, albumLabel, thumbnail } from './ui.js';
import { mountPhoto } from './library-photo.js';

const PAGE_SIZE = 60;
// Cached results older than this are loaded again when coming back to the grid
const CACHE_MAX_AGE_MS = 2 * 60 * 1000;

const SORTS = {
  neu: { label: 'Neueste zuerst', sort: 'added', order: 'desc' },
  alt: { label: 'Älteste zuerst', sort: 'added', order: 'asc' },
  aufnahme: { label: 'Aufnahmedatum', sort: 'taken', order: 'desc' },
  name: { label: 'Name', sort: 'name', order: 'asc' },
};
const STATUSES = { alle: 'Alle', sichtbar: 'Sichtbar', ausgeblendet: 'Ausgeblendet' };

// The router mounts the view anew for every hash. The loaded list survives
// here, so the detail view can step through it and the grid comes back
// where it was.
const cache = { key: null, items: [], total: 0, done: false, loadedAt: 0, scrollY: 0, lastId: null };
let pending = null;
let pendingKey = null;

const fileName = (media) => media.path.split('/').pop();
const displayName = (media) => media.title || fileName(media);

function readFilters(query) {
  return {
    album: query.get('album') ?? '',
    q: query.get('q') ?? '',
    status: STATUSES[query.get('status')] ? query.get('status') : 'alle',
    sort: SORTS[query.get('sort')] ? query.get('sort') : 'neu',
  };
}

const keyOf = (filters) => JSON.stringify(filters);
const isDefault = (filters) => !filters.album && !filters.q && filters.status === 'alle';

function hashFor(filters, id) {
  const params = new URLSearchParams();
  if (filters.album) params.set('album', filters.album);
  if (filters.q) params.set('q', filters.q);
  if (filters.status !== 'alle') params.set('status', filters.status);
  if (filters.sort !== 'neu') params.set('sort', filters.sort);
  const query = params.toString();
  return `#/mediathek${id ? `/${id}` : ''}${query ? `?${query}` : ''}`;
}

function mediaPath(filters, offset) {
  const { sort, order } = SORTS[filters.sort];
  const enabled = { sichtbar: 'true', ausgeblendet: 'false' }[filters.status] ?? '';
  return withQuery('/api/media', { album: filters.album, q: filters.q, enabled, sort, order, offset, limit: PAGE_SIZE });
}

function resetCache(filters) {
  Object.assign(cache, { key: keyOf(filters), items: [], total: 0, done: false, loadedAt: Date.now(), scrollY: 0 });
}

// Loads the next page into the cache. Resolves to true when items were added.
function loadMore(filters) {
  const key = keyOf(filters);
  if (cache.key !== key) resetCache(filters);
  if (cache.done) return Promise.resolve(false);
  if (pending && pendingKey === key) return pending;

  pendingKey = key;
  pending = (async () => {
    try {
      const page = await get(mediaPath(filters, cache.items.length));
      if (cache.key !== key) return false; // filters changed meanwhile
      const known = new Set(cache.items.map((media) => media.id));
      cache.items.push(...page.items.filter((media) => !known.has(media.id)));
      cache.total = page.total;
      cache.done = page.items.length < PAGE_SIZE || cache.items.length >= page.total;
      return page.items.length > 0;
    } finally {
      if (pendingKey === key) {
        pending = null;
        pendingKey = null;
      }
    }
  })();
  return pending;
}

export default {
  title: 'Mediathek',
  mount(root, context) {
    const filters = readFilters(context.query);
    if (context.params[0]) return mountDetail(root, context, filters, Number(context.params[0]));
    return mountGrid(root, context, filters);
  },
};

// Detail

function mountDetail(root, { navigate }, filters, id) {
  const key = keyOf(filters);
  const indexOf = (mediaId) => (cache.key === key ? cache.items.findIndex((media) => media.id === mediaId) : -1);
  cache.lastId = id;

  const nav = {
    position() {
      const index = indexOf(id);
      return {
        index,
        total: cache.total,
        hasPrev: index > 0,
        hasNext: index >= 0 && (index < cache.items.length - 1 || !cache.done),
      };
    },
    async step(direction) {
      const index = indexOf(id);
      if (index < 0) return null;
      const target = index + direction;
      if (target >= cache.items.length && !cache.done) await loadMore(filters);
      return cache.items[target]?.id ?? null;
    },
  };

  const index = indexOf(id);
  return mountPhoto(root, {
    id,
    media: index >= 0 ? cache.items[index] : null,
    nav,
    open: (otherId) => navigate(hashFor(filters, otherId)),
    close: () => navigate(hashFor(filters)),
    changed(media) {
      const position = indexOf(media.id);
      if (position >= 0) cache.items[position] = media;
    },
    deleted(mediaId) {
      const position = indexOf(mediaId);
      if (position < 0) return null;
      cache.items.splice(position, 1);
      cache.total = Math.max(0, cache.total - 1);
      return (cache.items[position] ?? cache.items[position - 1])?.id ?? null;
    },
  });
}

// Grid

function mountGrid(root, { navigate }, filters) {
  let alive = true;
  let albums = [];
  let selectMode = false;
  let anchor = null;
  const selected = new Set();

  const count = el('span', { className: 'badge' });
  const selectToggle = el('button', { type: 'button', className: 'secondary', textContent: 'Auswählen' });
  const refreshButton = el('button', {
    type: 'button', className: 'icon-button', textContent: '↻', title: 'Aktualisieren', attrs: { 'aria-label': 'Aktualisieren' },
  });
  const uploadLink = el('a', { className: 'button primary', textContent: 'Hochladen' });

  const albumSelect = el('select', { attrs: { 'aria-label': 'Album' } });
  const search = el('input', { type: 'search', placeholder: 'Titel, Ort, Datei …', value: filters.q, attrs: { 'aria-label': 'Suchen' } });
  const statusSelect = el('select', { attrs: { 'aria-label': 'Status' } },
    Object.entries(STATUSES).map(([value, label]) => el('option', { value, textContent: label })));
  const sortSelect = el('select', { attrs: { 'aria-label': 'Sortierung' } },
    Object.entries(SORTS).map(([value, sort]) => el('option', { value, textContent: sort.label })));
  statusSelect.value = filters.status;
  sortSelect.value = filters.sort;

  const grid = el('div', { className: 'thumb-grid lib-grid' });
  const sentinel = el('div', { className: 'lib-sentinel', attrs: { 'aria-hidden': 'true' } });
  const empty = el('div', { className: 'empty', hidden: true });
  const loading = el('p', { className: 'muted lib-loading', textContent: 'Lädt …', hidden: true });

  const selectionCount = el('span', { className: 'lib-selection-count' });
  const bulkButtons = {
    all: el('button', { type: 'button', className: 'small secondary', textContent: 'Alle geladenen' }),
    enable: el('button', { type: 'button', className: 'small', textContent: 'Einblenden' }),
    disable: el('button', { type: 'button', className: 'small', textContent: 'Ausblenden' }),
    move: el('button', { type: 'button', className: 'small', textContent: 'Verschieben …' }),
    remove: el('button', { type: 'button', className: 'small danger', textContent: 'Löschen' }),
    done: el('button', { type: 'button', className: 'small secondary', textContent: 'Fertig' }),
  };
  const bulkBar = el('div', { className: 'lib-bulkbar card', hidden: true, attrs: { role: 'toolbar', 'aria-label': 'Auswahl' } }, [
    selectionCount, bulkButtons.all, el('span', { className: 'spacer' }),
    bulkButtons.enable, bulkButtons.disable, bulkButtons.move, bulkButtons.remove, bulkButtons.done,
  ]);

  root.append(
    el('div', { className: 'page-header' }, [
      el('h1', { textContent: 'Mediathek' }), count, refreshButton, selectToggle, uploadLink,
    ]),
    el('div', { className: 'lib-filters' }, [albumSelect, search, statusSelect, sortSelect]),
    grid, loading, empty, sentinel, bulkBar,
  );

  // Header and filters

  function updateHeader() {
    count.textContent = `${cache.total.toLocaleString('de-DE')} ${cache.total === 1 ? 'Bild' : 'Bilder'}`;
    uploadLink.href = filters.album ? `#/hochladen?album=${encodeURIComponent(filters.album)}` : '#/hochladen';
  }

  function fillAlbums() {
    // The root folder is not offered: an album filter includes sub folders,
    // so "Hauptordner" would be the same as "Alle Alben"
    const options = [el('option', { value: '', textContent: 'Alle Alben' })];
    for (const album of albums) {
      if (album.name) options.push(el('option', { value: album.name, textContent: `${album.name} (${album.count})` }));
    }
    if (filters.album && !albums.some((album) => album.name === filters.album)) {
      options.push(el('option', { value: filters.album, textContent: filters.album }));
    }
    albumSelect.replaceChildren(...options);
    albumSelect.value = filters.album;
  }

  async function loadAlbums() {
    try {
      albums = await get('/api/albums');
      if (alive) fillAlbums();
    } catch (error) {
      toast(error.message, 'error');
    }
  }

  function applyFilters(changes) {
    Object.assign(filters, changes);
    history.replaceState(null, '', hashFor(filters));
    clearSelection();
    resetCache(filters);
    clear(grid);
    load();
  }

  // Tiles

  function tile(media) {
    const badges = [];
    if (media.enabled === false) badges.push(el('span', { className: 'badge warning', textContent: 'ausgeblendet' }));
    if (!media.renditions?.includes('thumb')) badges.push(el('span', { className: 'badge', textContent: 'Vorschau folgt' }));

    const node = el('button', {
      type: 'button',
      className: `lib-tile${media.enabled === false ? ' lib-dim' : ''}${selected.has(media.id) ? ' selected' : ''}`,
      dataset: { id: media.id },
      title: media.path,
      attrs: selectMode ? { 'aria-pressed': String(selected.has(media.id)) } : {},
    }, [
      el('span', { className: 'lib-thumb' }, [thumbnail(thumbUrl(media), displayName(media))]),
      el('span', { className: 'lib-check', attrs: { 'aria-hidden': 'true' } }),
      badges.length ? el('span', { className: 'lib-badges' }, badges) : null,
      el('span', { className: 'lib-caption' }, [
        el('strong', { textContent: displayName(media) }),
        el('span', { className: 'muted', textContent: albumLabel(media.album) }),
      ]),
    ]);
    return node;
  }

  function appendTiles(from) {
    grid.append(...cache.items.slice(from).map(tile));
  }

  function renderAll() {
    grid.replaceChildren(...cache.items.map(tile));
    updateEmpty();
  }

  function updateEmpty() {
    const none = cache.done && cache.items.length === 0;
    empty.hidden = !none;
    if (!none) return;
    if (isDefault(filters)) {
      empty.replaceChildren(
        el('p', { textContent: 'Noch keine Bilder in der Mediathek.' }),
        el('a', { className: 'button primary', href: '#/hochladen', textContent: 'Bilder hochladen' }),
      );
    } else {
      const reset = el('button', { type: 'button', textContent: 'Filter zurücksetzen' });
      reset.addEventListener('click', () => {
        search.value = '';
        statusSelect.value = 'alle';
        albumSelect.value = '';
        applyFilters({ album: '', q: '', status: 'alle' });
      });
      empty.replaceChildren(el('p', { textContent: 'Keine Bilder für diese Auswahl.' }), reset);
    }
  }

  // Sentinel still close to the viewport after appending: keep loading
  function sentinelNearby() {
    return sentinel.getBoundingClientRect().top < window.innerHeight + 600;
  }

  async function load() {
    if (!alive) return;
    loading.hidden = false;
    try {
      do {
        const before = cache.items.length;
        await loadMore(filters);
        if (!alive) return;
        appendTiles(before);
        updateHeader();
      } while (!cache.done && sentinelNearby());
    } catch (error) {
      if (alive) toast(error.message, 'error');
    } finally {
      if (alive) {
        loading.hidden = true;
        updateEmpty();
      }
    }
  }

  const observer = new IntersectionObserver((entries) => {
    if (entries.some((entry) => entry.isIntersecting) && !cache.done && !pending) load();
  }, { rootMargin: '600px 0px' });
  observer.observe(sentinel);

  // Selection

  function updateSelection() {
    const n = selected.size;
    selectionCount.textContent = n ? `${n} ausgewählt` : 'Nichts ausgewählt';
    for (const name of ['enable', 'disable', 'move', 'remove']) bulkButtons[name].disabled = n === 0;
    bulkBar.hidden = !selectMode;
    selectToggle.textContent = selectMode ? 'Fertig' : 'Auswählen';
    selectToggle.classList.toggle('primary', selectMode);
    grid.classList.toggle('lib-selecting', selectMode);
    for (const node of grid.children) {
      const isSelected = selected.has(Number(node.dataset.id));
      node.classList.toggle('selected', isSelected);
      if (selectMode) node.setAttribute('aria-pressed', String(isSelected));
      else node.removeAttribute('aria-pressed');
    }
  }

  function clearSelection() {
    selected.clear();
    anchor = null;
    updateSelection();
  }

  function setSelectMode(on) {
    selectMode = on;
    if (!on) selected.clear();
    anchor = null;
    updateSelection();
  }

  function toggleTile(index, range) {
    const id = cache.items[index].id;
    if (range && anchor !== null) {
      const [from, to] = anchor < index ? [anchor, index] : [index, anchor];
      for (let i = from; i <= to; i++) selected.add(cache.items[i].id);
    } else if (selected.has(id)) {
      selected.delete(id);
    } else {
      selected.add(id);
    }
    anchor = index;
    updateSelection();
  }

  grid.addEventListener('click', (event) => {
    const node = event.target.closest('.lib-tile');
    if (!node) return;
    const id = Number(node.dataset.id);
    const index = cache.items.findIndex((media) => media.id === id);
    if (index < 0) return;
    if (selectMode) {
      toggleTile(index, event.shiftKey);
    } else {
      cache.scrollY = window.scrollY;
      cache.lastId = id;
      navigate(hashFor(filters, id));
    }
  });

  // Bulk actions

  async function runBatch(action, extra, describe) {
    const ids = [...selected];
    if (!ids.length) return null;
    try {
      const result = await post('/api/media/batch', { ids, action, ...extra });
      const failed = result?.failed ?? [];
      toast(describe(result?.done ?? 0));
      if (failed.length) toast(`${failed.length} fehlgeschlagen: ${failed[0].error}`, 'error');
      return new Set(ids.filter((id) => !failed.some((failure) => failure.id === id)));
    } catch (error) {
      toast(error.message, 'error');
      return null;
    }
  }

  async function setEnabled(enabled) {
    const done = await runBatch(enabled ? 'enable' : 'disable', {}, (n) =>
      `${n} ${n === 1 ? 'Bild' : 'Bilder'} ${enabled ? 'eingeblendet' : 'ausgeblendet'}`);
    if (!done || !alive) return;
    for (const media of cache.items) if (done.has(media.id)) media.enabled = enabled;
    // Hidden pictures no longer match a "visible only" filter and vice versa
    if (filters.status !== 'alle') removeFromCache(done);
    clearSelection();
    renderAll();
  }

  function removeFromCache(ids) {
    const before = cache.items.length;
    cache.items = cache.items.filter((media) => !ids.has(media.id));
    cache.total = Math.max(0, cache.total - (before - cache.items.length));
    updateHeader();
  }

  async function removeSelected() {
    const n = selected.size;
    const ok = await confirm(
      n === 1 ? 'Bild löschen?' : `${n} Bilder löschen?`,
      'Die Bilder werden in den Papierkorb des Kiosks verschoben und verschwinden aus der Diashow.',
      { label: 'Löschen', danger: true },
    );
    if (!ok) return;
    const done = await runBatch('delete', {}, (count) => `${count} in den Papierkorb verschoben`);
    if (!done || !alive) return;
    removeFromCache(done);
    clearSelection();
    renderAll();
  }

  async function moveSelected() {
    const target = el('select', {}, [
      el('option', { value: '', textContent: albumLabel('') }),
      ...albums.filter((album) => album.name).map((album) => el('option', { value: album.name, textContent: album.name })),
    ]);
    if (filters.album) target.value = filters.album;
    const newAlbum = el('input', { placeholder: 'z. B. reisen/norwegen' });
    const body = el('div', { className: 'form-grid' }, [
      el('label', {}, ['In Album', target]),
      el('label', {}, ['Oder neues Album', newAlbum]),
    ]);
    const choice = await dialog({
      title: `${selected.size} ${selected.size === 1 ? 'Bild' : 'Bilder'} verschieben`,
      body,
      buttons: [{ label: 'Abbrechen', value: false }, { label: 'Verschieben', value: true, kind: 'primary' }],
    });
    if (choice !== true) return;

    let album = target.value;
    const typed = newAlbum.value.trim();
    if (typed) {
      try {
        album = (await post('/api/albums', { name: typed })).name;
      } catch (error) {
        toast(error.message, 'error');
        return;
      }
    }
    const done = await runBatch('move', { album }, (n) => `${n} nach „${albumLabel(album)}“ verschoben`);
    if (!done || !alive) return;

    const stillMatches = !filters.album || album === filters.album || album.startsWith(`${filters.album}/`);
    if (stillMatches) {
      for (const media of cache.items) {
        if (!done.has(media.id)) continue;
        media.album = album;
        media.path = album ? `${album}/${fileName(media)}` : fileName(media);
      }
    } else {
      removeFromCache(done);
    }
    clearSelection();
    renderAll();
    loadAlbums();
  }

  // Wiring

  selectToggle.addEventListener('click', () => setSelectMode(!selectMode));
  refreshButton.addEventListener('click', () => {
    resetCache(filters);
    clear(grid);
    loadAlbums();
    load();
  });
  bulkButtons.all.addEventListener('click', () => {
    for (const media of cache.items) selected.add(media.id);
    updateSelection();
  });
  bulkButtons.enable.addEventListener('click', () => setEnabled(true));
  bulkButtons.disable.addEventListener('click', () => setEnabled(false));
  bulkButtons.move.addEventListener('click', moveSelected);
  bulkButtons.remove.addEventListener('click', removeSelected);
  bulkButtons.done.addEventListener('click', () => setSelectMode(false));

  albumSelect.addEventListener('change', () => applyFilters({ album: albumSelect.value }));
  statusSelect.addEventListener('change', () => applyFilters({ status: statusSelect.value }));
  sortSelect.addEventListener('change', () => applyFilters({ sort: sortSelect.value }));
  const onSearch = debounce(() => {
    if (alive && search.value.trim() !== filters.q) applyFilters({ q: search.value.trim() });
  }, 350);
  search.addEventListener('input', onSearch);

  const onKey = (event) => {
    if (event.key === 'Escape' && selectMode && !document.querySelector('dialog[open]')) setSelectMode(false);
  };
  document.addEventListener('keydown', onKey);

  // Start: from the cache when coming back from a picture, else fresh
  fillAlbums();
  loadAlbums();
  updateHeader();
  updateSelection();
  const fresh = cache.key !== keyOf(filters) || Date.now() - cache.loadedAt > CACHE_MAX_AGE_MS || !cache.items.length;
  if (fresh) {
    resetCache(filters);
    load();
  } else {
    renderAll();
    const scrollY = cache.scrollY;
    const lastId = cache.lastId;
    requestAnimationFrame(() => {
      window.scrollTo(0, scrollY);
      const last = lastId && grid.querySelector(`[data-id="${lastId}"]`);
      if (last) {
        last.scrollIntoView({ block: 'nearest' });
        last.focus({ preventScroll: true });
      }
    });
    if (!cache.done && sentinelNearby()) load();
  }

  return () => {
    alive = false;
    cache.scrollY = window.scrollY;
    observer.disconnect();
    document.removeEventListener('keydown', onKey);
  };
}
