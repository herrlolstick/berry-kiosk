// Grid editor for one layout: canvas with draggable regions, toolbar and
// properties panel. The view owns loading and saving; this module only edits
// the layout object it is given and reports every change through onChange.

import { thumbUrl } from './api.js';
import { albumLabel, el } from './ui.js';
import {
  INFO_FIELDS, REGION_TYPES, TRANSITIONS, clampInt, clone, fitIntoGrid, gridOf, newRegion, uniqueId, zOf,
} from './layout-model.js';

const HANDLES = ['n', 'e', 's', 'w', 'ne', 'nw', 'se', 'sw'];
const HISTORY_LIMIT = 100;

// options: { layout, albums, aspect (width / height), onChange(layout) }
export function createLayoutEditor({ layout, albums = [], aspect = 16 / 9, onChange = () => {} }) {
  let current = clone(layout);
  let selectedId = current.regions?.[0]?.id ?? null;
  let past = [];
  let future = [];
  let clockTimer = null;
  const boxes = new Map();
  const covers = new Map(albums.filter((album) => album.cover).map((album) => [album.name, album.cover]));

  const canvas = el('div', {
    className: 'lay-canvas',
    tabIndex: 0,
    attrs: { role: 'application', 'aria-label': 'Layout-Raster. Pfeiltasten verschieben, Umschalt+Pfeiltasten ändern die Größe, Entf löscht.' },
  });
  setAspect(aspect);
  const columnsInput = el('input', { type: 'number', min: 1, max: 48, className: 'lay-grid-input' });
  const rowsInput = el('input', { type: 'number', min: 1, max: 48, className: 'lay-grid-input' });
  const undoButton = button('↶', 'Rückgängig (Strg+Z)', undo);
  const redoButton = button('↷', 'Wiederholen (Strg+Umschalt+Z)', redo);
  const regionButtons = [
    button('Duplizieren', 'Region duplizieren', duplicateSelected),
    button('Nach vorn', 'Eine Ebene nach vorn', () => shiftZ(1)),
    button('Nach hinten', 'Eine Ebene nach hinten', () => shiftZ(-1)),
    button('Löschen', 'Region löschen (Entf)', removeSelected, 'danger secondary'),
  ];
  const panel = el('aside', { className: 'lay-panel card' });

  const toolbar = el('div', { className: 'lay-toolbar' }, [
    el('div', { className: 'row' }, [
      el('span', { className: 'muted small', textContent: 'Hinzufügen:' }),
      ...Object.entries(REGION_TYPES).map(([type, info]) => button(`+ ${info.label}`, `${info.label} hinzufügen`, () => addRegion(type))),
    ]),
    el('div', { className: 'row' }, [
      ...regionButtons,
      el('span', { className: 'spacer' }),
      el('label', { className: 'inline small' }, ['Spalten', columnsInput]),
      el('label', { className: 'inline small' }, ['Zeilen', rowsInput]),
      undoButton,
      redoButton,
    ]),
  ]);

  const element = el('div', { className: 'lay-editor' }, [
    toolbar,
    el('div', { className: 'lay-workspace' }, [el('div', { className: 'lay-stage' }, [canvas]), panel]),
  ]);


  // Width follows from the height limit for tall (portrait) screens
  function setAspect(value) {
    canvas.style.aspectRatio = String(value);
    canvas.style.setProperty('--lay-aspect', String(value));
  }

  function button(text, title, onclick, className = 'small secondary') {
    return el('button', { type: 'button', className, textContent: text, title, onclick });
  }

  // History: every change is one step

  function change(mutate, { panel: rebuildPanel = true } = {}) {
    const before = JSON.stringify(current);
    mutate();
    if (JSON.stringify(current) === before) return;
    past.push(before);
    if (past.length > HISTORY_LIMIT) past.shift();
    future = [];
    afterChange(rebuildPanel);
  }

  function afterChange(rebuildPanel = true) {
    if (selectedId && !regionById(selectedId)) selectedId = null;
    renderCanvas();
    if (rebuildPanel) renderPanel();
    updateButtons();
    onChange(clone(current));
  }

  function undo() {
    if (!past.length) return;
    future.push(JSON.stringify(current));
    current = JSON.parse(past.pop());
    afterChange();
  }

  function redo() {
    if (!future.length) return;
    past.push(JSON.stringify(current));
    current = JSON.parse(future.pop());
    afterChange();
  }

  // Regions

  const regions = () => (current.regions ??= []);
  const regionById = (id) => regions().find((region) => region.id === id);
  const selected = () => regionById(selectedId);
  const slideshowIds = () => regions().filter((region) => region.type === 'slideshow').map((region) => region.id);

  function select(id) {
    if (selectedId === id) return;
    selectedId = id;
    for (const [boxId, box] of boxes) box.classList.toggle('selected', boxId === id);
    renderPanel();
    updateButtons();
  }

  function addRegion(type) {
    const region = newRegion(type, current);
    change(() => regions().push(region));
    select(region.id);
    canvas.focus();
  }

  function duplicateSelected() {
    const source = selected();
    if (!source) return;
    const copy = clone(source);
    copy.id = uniqueId(source.id, regions().map((region) => region.id));
    const grid = gridOf(current);
    if (copy.x + copy.w < grid.columns) copy.x += 1;
    else if (copy.y + copy.h < grid.rows) copy.y += 1;
    change(() => regions().push(copy));
    select(copy.id);
  }

  function removeSelected() {
    const region = selected();
    if (!region) return;
    change(() => {
      current.regions = regions().filter((item) => item !== region);
      // Widgets that followed it fall back to the first slideshow
      for (const item of regions()) if (item.follows === region.id) delete item.follows;
    });
    canvas.focus();
  }

  function shiftZ(delta) {
    const region = selected();
    if (!region) return;
    change(() => { region.z = Math.max(0, zOf(region) + delta); });
  }

  // Changing the grid scales regions along, so the picture stays the same
  function resizeGrid(columnsValue, rowsValue) {
    const old = gridOf(current);
    const columns = clampInt(columnsValue, 1, 48);
    const rows = clampInt(rowsValue, 1, 48);
    change(() => {
      current.grid = { columns, rows };
      for (const region of regions()) {
        const x2 = Math.round(((region.x + region.w) * columns) / old.columns);
        const y2 = Math.round(((region.y + region.h) * rows) / old.rows);
        region.x = Math.round((region.x * columns) / old.columns);
        region.y = Math.round((region.y * rows) / old.rows);
        region.w = Math.max(1, x2 - region.x);
        region.h = Math.max(1, y2 - region.y);
        fitIntoGrid(region, current.grid);
      }
    });
  }

  // Canvas

  function render() {
    renderCanvas();
    renderPanel();
    updateButtons();
  }

  function renderCanvas() {
    const { columns, rows } = gridOf(current);
    columnsInput.value = columns;
    rowsInput.value = rows;
    canvas.style.setProperty('--lay-columns', columns);
    canvas.style.setProperty('--lay-rows', rows);
    canvas.style.backgroundSize = `${100 / columns}% ${100 / rows}%`;

    const ids = new Set(regions().map((region) => region.id));
    for (const [id, box] of boxes) {
      if (!ids.has(id)) {
        box.remove();
        boxes.delete(id);
      }
    }
    regions().forEach((region, index) => {
      let box = boxes.get(region.id);
      if (!box) {
        box = createBox(region.id);
        boxes.set(region.id, box);
        canvas.append(box);
      }
      updateBox(box, region, index);
    });
  }

  function createBox(id) {
    const box = el('div', { className: 'lay-box', tabIndex: -1, dataset: { id } }, [
      el('div', { className: 'lay-box-content' }),
      el('span', { className: 'lay-box-label' }),
      ...HANDLES.map((handle) => el('span', { className: `lay-handle lay-handle-${handle}`, dataset: { handle } })),
    ]);
    box.addEventListener('pointerdown', (event) => startDrag(event, box));
    return box;
  }

  function updateBox(box, region, index) {
    const { columns, rows } = gridOf(current);
    box.style.left = `${(region.x / columns) * 100}%`;
    box.style.top = `${(region.y / rows) * 100}%`;
    box.style.width = `${(region.w / columns) * 100}%`;
    box.style.height = `${(region.h / rows) * 100}%`;
    box.style.zIndex = String(zOf(region) * 100 + index + 1);
    box.dataset.type = region.type;
    box.dataset.id = region.id;
    box.classList.toggle('selected', region.id === selectedId);
    box.setAttribute('aria-label', `${REGION_TYPES[region.type]?.label ?? region.type} „${region.id}“`);
    box.querySelector('.lay-box-label').textContent = `${REGION_TYPES[region.type]?.label ?? region.type} · ${region.id} · Ebene ${zOf(region)}`;
    fillContent(box.querySelector('.lay-box-content'), region);
  }

  // What the region roughly shows on the kiosk
  function fillContent(node, region) {
    const key = `${region.type}|${region.album ?? ''}|${region.fit ?? ''}|${region.format ?? ''}|${region.showDate ?? ''}|${region.style?.align ?? ''}`;
    if (node.dataset.key === key) return;
    node.dataset.key = key;
    node.dataset.align = region.style?.align ?? '';

    if (region.type === 'slideshow') {
      const cover = covers.get(region.album ?? '') ?? (!region.album ? covers.values().next().value : undefined);
      node.replaceChildren(cover
        ? el('img', { src: thumbUrl(cover), alt: '', className: region.fit === 'contain' ? 'contain' : '' })
        : el('span', { className: 'lay-box-text', textContent: albumLabel(region.album) === 'Hauptordner' ? 'Alle Bilder' : albumLabel(region.album) }));
    } else if (region.type === 'clock') {
      node.replaceChildren(el('strong', { className: 'lay-clock-time', textContent: clockText(region) }),
        region.showDate === false ? null : el('span', { className: 'lay-box-text', textContent: new Date().toLocaleDateString('de-DE', { weekday: 'long', day: 'numeric', month: 'long' }) }));
    } else if (region.type === 'imageInfo') {
      node.replaceChildren(el('strong', { textContent: 'Titel' }), el('span', { className: 'lay-box-text', textContent: 'Ort · Datum' }));
    } else {
      node.replaceChildren(el('em', { className: 'lay-box-text', textContent: '„Ein Zitat zum Bild …“' }));
    }
  }

  function clockText(region) {
    const options = { hour: '2-digit', minute: '2-digit' };
    if ((region.format ?? 'HH:mm').includes('s')) options.second = '2-digit';
    return new Date().toLocaleTimeString('de-DE', options);
  }

  function updateClocks() {
    for (const region of regions()) {
      if (region.type !== 'clock') continue;
      const time = boxes.get(region.id)?.querySelector('.lay-clock-time');
      if (time) time.textContent = clockText(region);
    }
  }

  // Dragging: move, or resize by one of the handles, snapping to cells

  function startDrag(event, box) {
    if (event.button !== 0) return;
    const region = regionById(box.dataset.id);
    if (!region) return;
    event.preventDefault();
    event.stopPropagation();
    select(region.id);
    canvas.focus({ preventScroll: true });

    const handle = event.target.dataset.handle ?? '';
    const rect = canvas.getBoundingClientRect();
    const { columns, rows } = gridOf(current);
    const cellW = rect.width / columns;
    const cellH = rect.height / rows;
    const start = { x: region.x, y: region.y, w: region.w, h: region.h };
    const before = JSON.stringify(current);
    box.setPointerCapture(event.pointerId);
    box.classList.add('dragging');

    const move = (moveEvent) => {
      const dx = Math.round((moveEvent.clientX - event.clientX) / cellW);
      const dy = Math.round((moveEvent.clientY - event.clientY) / cellH);
      Object.assign(region, dragged(start, handle, dx, dy, columns, rows));
      updateBox(box, region, regions().indexOf(region));
    };
    const end = () => {
      box.removeEventListener('pointermove', move);
      box.removeEventListener('pointerup', end);
      box.removeEventListener('pointercancel', end);
      box.classList.remove('dragging');
      if (JSON.stringify(current) !== before) {
        past.push(before);
        future = [];
        afterChange();
      }
    };
    box.addEventListener('pointermove', move);
    box.addEventListener('pointerup', end);
    box.addEventListener('pointercancel', end);
  }

  // Keyboard

  function onKey(event) {
    const meta = event.ctrlKey || event.metaKey;
    if (meta && event.key.toLowerCase() === 'z') {
      event.preventDefault();
      if (event.shiftKey) redo();
      else undo();
      return;
    }
    const region = selected();
    if (!region) return;
    if (event.key === 'Delete' || event.key === 'Backspace') {
      event.preventDefault();
      removeSelected();
      return;
    }
    const steps = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] }[event.key];
    if (!steps) return;
    event.preventDefault();
    const { columns, rows } = gridOf(current);
    const start = { x: region.x, y: region.y, w: region.w, h: region.h };
    change(() => Object.assign(region, dragged(start, event.shiftKey ? 'se' : '', steps[0], steps[1], columns, rows)));
  }

  // Properties panel

  function renderPanel() {
    const region = selected();
    if (!region) {
      panel.replaceChildren(
        el('h2', { textContent: 'Eigenschaften' }),
        el('p', { className: 'muted', textContent: regions().length ? 'Wähle eine Region im Raster aus.' : 'Füge oben eine Diashow oder ein Widget hinzu.' }),
      );
      return;
    }
    panel.replaceChildren(
      el('h2', { textContent: `${REGION_TYPES[region.type]?.label ?? region.type}` }),
      ...commonFields(region),
      ...typeFields(region),
      ...(region.type === 'slideshow' ? [] : [styleFields(region)]),
    );
  }

  // Sets region[path] (dotted for nested objects); '' or undefined removes the key
  function setProp(region, path, value) {
    change(() => {
      const keys = path.split('.');
      let target = region;
      for (const key of keys.slice(0, -1)) target = target[key] ??= {};
      const last = keys[keys.length - 1];
      if (value === '' || value === undefined || value === null) delete target[last];
      else target[last] = value;
      if (keys.length > 1 && Object.keys(target).length === 0) delete region[keys[0]];
    }, { panel: false });
  }

  function commonFields(region) {
    const idInput = el('input', { value: region.id, autocomplete: 'off', spellcheck: false });
    idInput.addEventListener('change', () => renameRegion(region, idInput));
    const grid = gridOf(current);
    const number = (key, label, min, max) => {
      const input = el('input', { type: 'number', min, max, value: region[key] });
      input.addEventListener('change', () => {
        change(() => {
          region[key] = clampInt(input.value, min, max);
          fitIntoGrid(region, gridOf(current));
        });
      });
      return el('label', {}, [label, input]);
    };
    return [
      el('label', {}, ['ID', idInput]),
      el('div', { className: 'lay-fields four' }, [
        number('x', 'Spalte', 0, grid.columns - 1),
        number('y', 'Zeile', 0, grid.rows - 1),
        number('w', 'Breite', 1, grid.columns),
        number('h', 'Höhe', 1, grid.rows),
      ]),
    ];
  }

  function renameRegion(region, input) {
    const id = input.value.trim().replace(/\s+/g, '-');
    if (!id || (id !== region.id && regionById(id))) {
      input.value = region.id;
      input.setCustomValidity(id ? 'Diese ID gibt es schon.' : 'Die ID darf nicht leer sein.');
      input.reportValidity();
      return;
    }
    input.setCustomValidity('');
    const old = region.id;
    change(() => {
      region.id = id;
      selectedId = id;
      for (const item of regions()) if (item.follows === old) item.follows = id;
    });
  }

  function selectField(label, value, options, onSelect) {
    const select = el('select', {}, options.map(([optionValue, text]) => el('option', { value: optionValue, textContent: text, selected: optionValue === value })));
    select.addEventListener('change', () => onSelect(select.value));
    return el('label', {}, [label, select]);
  }

  function numberField(label, value, min, max, onSet, step = 1) {
    const input = el('input', { type: 'number', min, max, step, value: value ?? '' });
    input.addEventListener('change', () => onSet(input.value === '' ? undefined : Math.min(max, Math.max(min, Number(input.value)))));
    return el('label', {}, [label, input]);
  }

  function textField(label, value, onSet, placeholder = '') {
    const input = el('input', { value: value ?? '', placeholder, autocomplete: 'off' });
    input.addEventListener('change', () => onSet(input.value.trim()));
    return el('label', {}, [label, input]);
  }

  function checkField(label, checked, onSet) {
    const input = el('input', { type: 'checkbox', checked });
    input.addEventListener('change', () => onSet(input.checked));
    return el('label', { className: 'inline' }, [input, label]);
  }

  function followsField(region) {
    const options = [['', 'Erste Diashow'], ...slideshowIds().map((id) => [id, id])];
    return selectField('Folgt der Diashow', region.follows ?? '', options, (value) => setProp(region, 'follows', value));
  }

  function typeFields(region) {
    const set = (path) => (value) => setProp(region, path, value);
    if (region.type === 'slideshow') {
      const albumOptions = [['', 'Alle Bilder'], ...albums.filter((album) => album.name).map((album) => [album.name, `${album.name} (${album.count})`])];
      if (region.album && !albums.some((album) => album.name === region.album)) albumOptions.push([region.album, `${region.album} (fehlt)`]);
      return [
        selectField('Album', region.album ?? '', albumOptions, (value) => setProp(region, 'album', value || '')),
        el('div', { className: 'lay-fields' }, [
          selectField('Reihenfolge', region.order ?? 'sequential', [['shuffle', 'Zufällig'], ['sequential', 'Der Reihe nach']], set('order')),
          numberField('Anzeigedauer (s)', region.interval ?? 15, 1, 3600, set('interval')),
        ]),
        selectField('Einpassen', region.fit ?? 'cover', [['cover', 'Füllen (zuschneiden)'], ['contain', 'Ganz zeigen']], set('fit')),
        el('div', { className: 'lay-fields' }, [
          selectField('Übergang', region.transition?.type ?? 'fade', TRANSITIONS, set('transition.type')),
          numberField('Dauer (ms)', region.transition?.duration ?? 1200, 0, 10000, set('transition.duration'), 100),
        ]),
        el('button', { type: 'button', className: 'small secondary', textContent: 'Übergang auf alle Diashows anwenden', onclick: () => applyTransitionToAll(region) }),
      ];
    }
    if (region.type === 'clock') {
      return [
        selectField('Format', region.format ?? 'HH:mm', [['HH:mm', '14:05'], ['HH:mm:ss', '14:05:09'], ['h:mm AP', '2:05 PM']], set('format')),
        checkField('Datum zeigen', region.showDate !== false, (value) => setProp(region, 'showDate', value ? undefined : false)),
        textField('Datumsformat', region.dateFormat, set('dateFormat'), 'dddd, d. MMMM'),
      ];
    }
    if (region.type === 'imageInfo') {
      const fields = region.fields ?? ['title', 'location', 'date'];
      return [
        followsField(region),
        el('fieldset', { className: 'lay-checks' }, [
          el('legend', { textContent: 'Angaben' }),
          ...INFO_FIELDS.map(([key, label]) => checkField(label, fields.includes(key), (on) => {
            const next = INFO_FIELDS.map(([name]) => name).filter((name) => (name === key ? on : fields.includes(name)));
            setProp(region, 'fields', next);
            renderPanel();
          })),
        ]),
        textField('Datumsformat', region.dateFormat, set('dateFormat'), 'd. MMMM yyyy'),
      ];
    }
    return [
      followsField(region),
      selectField('Quelle', region.source ?? 'image-or-global', [
        ['image-or-global', 'Zitat des Bildes, sonst aus der Liste'],
        ['image', 'Nur Zitate der Bilder'],
        ['global', 'Nur aus der Zitatliste'],
      ], set('source')),
      numberField('Wechsel ohne Diashow (s)', region.interval ?? 60, 5, 3600, set('interval')),
    ];
  }

  function styleFields(region) {
    const style = region.style ?? {};
    const set = (key) => (value) => setProp(region, `style.${key}`, value);
    return el('details', { className: 'lay-style', open: true }, [
      el('summary', { textContent: 'Darstellung' }),
      el('div', { className: 'lay-fields' }, [
        selectField('Hintergrund', style.background ?? 'dark', [['dark', 'Dunkle Fläche'], ['light', 'Helle Fläche'], ['none', 'Ohne (Schatten)']], set('background')),
        selectField('Ausrichtung', style.align ?? '', [['', 'Standard'], ['left', 'Links'], ['center', 'Mitte'], ['right', 'Rechts']], set('align')),
      ]),
      el('div', { className: 'lay-fields' }, [
        textField('Textfarbe', style.color, set('color'), 'z. B. #ffffff'),
        textField('Schrift', style.font, set('font'), 'Systemschrift'),
      ]),
      el('div', { className: 'lay-fields' }, [
        numberField('Deckkraft (0–1)', style.opacity, 0.1, 1, set('opacity'), 0.05),
        numberField('Einblenden (ms)', region.fadeDuration, 0, 5000, (value) => setProp(region, 'fadeDuration', value), 100),
      ]),
      region.type === 'quote' ? checkField('Kursiv', style.italic !== false, (value) => set('italic')(value ? undefined : false)) : null,
    ]);
  }

  function applyTransitionToAll(source) {
    const transition = clone(source.transition ?? { type: 'fade', duration: 1200 });
    change(() => {
      for (const region of regions()) if (region.type === 'slideshow') region.transition = clone(transition);
    });
  }

  function updateButtons() {
    undoButton.disabled = past.length === 0;
    redoButton.disabled = future.length === 0;
    for (const item of regionButtons) item.disabled = !selected();
  }

  // Wired up last: the helpers above are const arrow functions
  columnsInput.addEventListener('change', () => resizeGrid(columnsInput.value, gridOf(current).rows));
  rowsInput.addEventListener('change', () => resizeGrid(gridOf(current).columns, rowsInput.value));
  canvas.addEventListener('keydown', onKey);
  canvas.addEventListener('pointerdown', (event) => {
    if (event.target === canvas) select(null);
  });
  clockTimer = setInterval(updateClocks, 30000);
  render();

  return {
    element,
    layout: () => clone(current),
    // Replaces the layout (e.g. after "Verwerfen") and starts a fresh history
    setLayout(next) {
      current = clone(next);
      past = [];
      future = [];
      if (!selected()) selectedId = current.regions?.[0]?.id ?? null;
      for (const box of boxes.values()) box.remove();
      boxes.clear();
      render();
    },
    setAspect,
    destroy() {
      clearInterval(clockTimer);
      element.remove();
    },
  };
}

// New position and size after dragging by dx/dy cells
function dragged(start, handle, dx, dy, columns, rows) {
  let { x, y, w, h } = start;
  if (!handle) {
    x = Math.min(columns - w, Math.max(0, x + dx));
    y = Math.min(rows - h, Math.max(0, y + dy));
    return { x, y, w, h };
  }
  if (handle.includes('e')) w = Math.min(columns - x, Math.max(1, start.w + dx));
  if (handle.includes('s')) h = Math.min(rows - y, Math.max(1, start.h + dy));
  if (handle.includes('w')) {
    const left = Math.min(start.x + start.w - 1, Math.max(0, start.x + dx));
    w = start.x + start.w - left;
    x = left;
  }
  if (handle.includes('n')) {
    const top = Math.min(start.y + start.h - 1, Math.max(0, start.y + dy));
    h = start.y + start.h - top;
    y = top;
  }
  return { x, y, w, h };
}
