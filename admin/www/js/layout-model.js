// Layout data for the editor: defaults, templates, validation. Mirrors what
// display/qml/LayoutView.qml and the region components read.

export const REGION_TYPES = {
  slideshow: { label: 'Diashow', size: [12, 12] },
  clock: { label: 'Uhr', size: [3, 2] },
  imageInfo: { label: 'Bildinfo', size: [5, 2] },
  quote: { label: 'Zitat', size: [6, 2] },
};

export const TRANSITIONS = [
  ['fade', 'Überblenden'],
  ['slide-left', 'Nach links schieben'],
  ['slide-right', 'Nach rechts schieben'],
  ['slide-up', 'Nach oben schieben'],
  ['slide-down', 'Nach unten schieben'],
  ['zoom', 'Zoomen'],
  ['kenburns', 'Ken Burns (langsamer Schwenk)'],
  ['mix', 'Zufällig gemischt'],
  ['none', 'Ohne Übergang'],
];

export const INFO_FIELDS = [
  ['title', 'Titel'],
  ['location', 'Ort'],
  ['date', 'Datum'],
  ['description', 'Beschreibung'],
  ['camera', 'Kamera'],
  ['lens', 'Objektiv'],
  ['exposure', 'Belichtung'],
];

export const clone = (value) => JSON.parse(JSON.stringify(value));

export function slugify(text) {
  const slug = String(text)
    .toLowerCase()
    .replace(/ä/g, 'ae').replace(/ö/g, 'oe').replace(/ü/g, 'ue').replace(/ß/g, 'ss')
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '');
  return slug || 'layout';
}

export function uniqueId(base, taken) {
  const used = new Set(taken);
  if (!used.has(base)) return base;
  for (let n = 2; ; n++) {
    if (!used.has(`${base}-${n}`)) return `${base}-${n}`;
  }
}

export const gridOf = (layout) => ({
  columns: clampInt(layout.grid?.columns ?? 12, 1, 48),
  rows: clampInt(layout.grid?.rows ?? 12, 1, 48),
});

export function clampInt(value, min, max) {
  const number = Math.round(Number(value));
  return Math.min(max, Math.max(min, Number.isFinite(number) ? number : min));
}

// A new region of the given type, placed sensibly in the grid
export function newRegion(type, layout) {
  const { columns, rows } = gridOf(layout);
  const [w0, h0] = REGION_TYPES[type].size;
  const w = Math.min(columns, Math.max(1, Math.round((w0 / 12) * columns)));
  const h = Math.min(rows, Math.max(1, Math.round((h0 / 12) * rows)));
  const taken = (layout.regions ?? []).map((region) => region.id);
  const base = { slideshow: 'diashow', clock: 'uhr', imageInfo: 'bildinfo', quote: 'zitat' }[type];
  const region = { id: uniqueId(base, taken), type, x: 0, y: 0, w, h };

  if (type === 'slideshow') {
    Object.assign(region, { album: '', order: 'shuffle', interval: 20, fit: 'cover', transition: { type: 'fade', duration: 1500 } });
  } else {
    region.z = 1;
    if (type === 'clock') Object.assign(region, { x: columns - w, format: 'HH:mm', showDate: true, style: { background: 'none', align: 'right' } });
    if (type === 'imageInfo') Object.assign(region, { y: rows - h, fields: ['title', 'location', 'date'], style: { background: 'dark' } });
    if (type === 'quote') Object.assign(region, { x: columns - w, y: rows - h, source: 'image-or-global', style: { background: 'none', align: 'right' } });
  }
  return region;
}

function region(type, id, x, y, w, h, extra = {}) {
  const base = newRegion(type, { grid: { columns: 12, rows: 12 }, regions: [] });
  return { ...base, id, x, y, w, h, ...extra };
}

export const TEMPLATES = [
  {
    key: 'empty',
    label: 'Leer',
    make: () => ({ grid: { columns: 12, rows: 12 }, regions: [] }),
  },
  {
    key: 'full',
    label: 'Vollbild',
    make: () => ({ grid: { columns: 12, rows: 12 }, regions: [region('slideshow', 'main', 0, 0, 12, 12)] }),
  },
  {
    key: 'full-widgets',
    label: 'Vollbild mit Uhr und Bildinfo',
    make: () => ({
      grid: { columns: 12, rows: 12 },
      regions: [
        region('slideshow', 'main', 0, 0, 12, 12),
        region('clock', 'uhr', 9, 0, 3, 2),
        region('imageInfo', 'bildinfo', 0, 10, 5, 2, { follows: 'main' }),
      ],
    }),
  },
  {
    key: 'collage',
    label: 'Collage 1+2',
    make: () => ({
      grid: { columns: 12, rows: 12 },
      regions: [
        region('slideshow', 'gross', 0, 0, 8, 12, { interval: 25 }),
        region('slideshow', 'oben', 8, 0, 4, 6, { interval: 17 }),
        region('slideshow', 'unten', 8, 6, 4, 6, { interval: 23 }),
      ],
    }),
  },
  {
    key: 'portrait-pair',
    label: 'Hochformat-Paar',
    make: () => ({
      grid: { columns: 12, rows: 12 },
      regions: [
        region('slideshow', 'links', 0, 0, 6, 12, { fit: 'contain' }),
        region('slideshow', 'rechts', 6, 0, 6, 12, { fit: 'contain', interval: 23 }),
      ],
    }),
  },
];

// Keeps a region inside the grid with at least one cell
export function fitIntoGrid(region, grid) {
  region.w = clampInt(region.w, 1, grid.columns);
  region.h = clampInt(region.h, 1, grid.rows);
  region.x = clampInt(region.x, 0, grid.columns - region.w);
  region.y = clampInt(region.y, 0, grid.rows - region.h);
  return region;
}

export const zOf = (region) => region.z ?? (region.type === 'slideshow' ? 0 : 1);

// List of problems (German), empty when the layout can be saved
export function validateLayout(layout) {
  const problems = [];
  const { columns, rows } = gridOf(layout);
  const ids = new Set();
  const slideshows = new Set((layout.regions ?? []).filter((r) => r.type === 'slideshow').map((r) => r.id));

  for (const r of layout.regions ?? []) {
    const name = `„${r.id || '?'}“`;
    if (!r.id) problems.push('Jede Region braucht eine ID.');
    else if (ids.has(r.id)) problems.push(`Die ID ${name} gibt es mehrfach.`);
    ids.add(r.id);
    if (!REGION_TYPES[r.type]) problems.push(`${name}: unbekannter Typ.`);
    for (const key of ['x', 'y', 'w', 'h']) {
      if (!Number.isInteger(r[key])) problems.push(`${name}: ${key} muss eine ganze Zahl sein.`);
    }
    if (r.x < 0 || r.y < 0 || r.w < 1 || r.h < 1 || r.x + r.w > columns || r.y + r.h > rows) {
      problems.push(`${name} liegt außerhalb des Rasters.`);
    }
    if (r.follows && !slideshows.has(r.follows)) problems.push(`${name} folgt der Diashow „${r.follows}“, die es nicht gibt.`);
  }
  return problems;
}
