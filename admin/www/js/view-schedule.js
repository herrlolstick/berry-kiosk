// Zeitplan: config.json "schedule" – which layout runs when, and when the
// screen is off. The rules mean exactly what core/schedule.cpp makes of them:
// later rules win where they overlap, a "to" before "from" runs past
// midnight, from == to is the whole day.

import { get, put } from './api.js';
import { el, toast, confirm, dialog } from './ui.js';

const DAY_LABELS = ['Mo', 'Di', 'Mi', 'Do', 'Fr', 'Sa', 'So'];
const DAY_KEYS = ['mo', 'di', 'mi', 'do', 'fr', 'sa', 'so'];
const DAY_NAMES = {
  mo: 1, mon: 1, montag: 1, monday: 1,
  di: 2, tue: 2, dienstag: 2, tuesday: 2,
  mi: 3, wed: 3, mittwoch: 3, wednesday: 3,
  do: 4, thu: 4, donnerstag: 4, thursday: 4,
  fr: 5, fri: 5, freitag: 5, friday: 5,
  sa: 6, sat: 6, samstag: 6, saturday: 6,
  so: 7, sun: 7, sonntag: 7, sunday: 7,
};
const MINUTES_PER_DAY = 24 * 60;
const PREVIEW_STEP = 5; // minutes per sample in the week preview

// Days

function dayNumber(name) {
  const text = String(name).trim().toLowerCase();
  if (/^\d+$/.test(text)) {
    const number = Number(text);
    return number >= 1 && number <= 7 ? number : 0;
  }
  return DAY_NAMES[text] || 0;
}

// "mo-fr", "sa,so", "fr-mo", "daily", [1, 2] or ["mon", "tue"] -> Set of 1…7, or null
function parseDays(value) {
  if (value === undefined || value === null || value === '') return new Set([1, 2, 3, 4, 5, 6, 7]);
  if (Array.isArray(value)) {
    const days = value.map((day) => (typeof day === 'number' ? (day >= 1 && day <= 7 ? day : 0) : dayNumber(day)));
    return days.length && !days.includes(0) ? new Set(days) : null;
  }
  const spec = String(value).trim().toLowerCase();
  if (['daily', 'täglich', 'taeglich', '*'].includes(spec)) return new Set([1, 2, 3, 4, 5, 6, 7]);
  const days = new Set();
  for (const part of spec.split(/[,\s]+/).filter(Boolean)) {
    const range = part.split('-');
    if (range.length === 1) {
      const day = dayNumber(range[0]);
      if (!day) return null;
      days.add(day);
    } else if (range.length === 2) {
      const first = dayNumber(range[0]);
      const last = dayNumber(range[1]);
      if (!first || !last) return null;
      for (let day = first; ; day = (day % 7) + 1) {
        days.add(day);
        if (day === last) break;
      }
    } else {
      return null;
    }
  }
  return days.size ? days : null;
}

// Set of days -> the shortest readable string: "daily", "mo-fr", "sa,so", "fr-mo"
function formatDays(days) {
  if (days.size === 7) return 'daily';
  const sorted = [...days].sort((a, b) => a - b);
  const runs = [];
  for (const day of sorted) {
    const run = runs[runs.length - 1];
    if (run && day === run[1] + 1) run[1] = day;
    else runs.push([day, day]);
  }
  // A run ending on Sunday continues into one starting on Monday
  if (runs.length > 1 && runs[0][0] === 1 && runs[runs.length - 1][1] === 7) {
    const last = runs.pop();
    runs[0] = [last[0], runs[0][1]];
  }
  return runs.map(([first, last]) => {
    const length = ((last - first + 7) % 7) + 1;
    if (length === 1) return DAY_KEYS[first - 1];
    if (length === 2) return `${DAY_KEYS[first - 1]},${DAY_KEYS[last - 1]}`;
    return `${DAY_KEYS[first - 1]}-${DAY_KEYS[last - 1]}`;
  }).join(',');
}

// Times

function toMinutes(text) {
  const match = /^(\d{1,2}):(\d{2})$/.exec(String(text ?? '').trim());
  if (!match) return null;
  const hours = Number(match[1]);
  const minutes = Number(match[2]);
  return hours < 24 && minutes < 60 ? hours * 60 + minutes : null;
}

const pad = (number) => String(number).padStart(2, '0');
const formatMinutes = (minutes) => `${pad(Math.floor(minutes / 60))}:${pad(minutes % 60)}`;

// Rules, as the editor keeps them

function ruleFromConfig(entry) {
  const { days, from, to, layout, screen, ...extra } = entry || {};
  const parsedDays = parseDays(days);
  const screenOff = ['off', 'aus'].includes(String(screen ?? '').trim().toLowerCase());
  return {
    days: parsedDays || new Set(),
    daysInvalid: parsedDays === null,
    from: toMinutes(from ?? '00:00') === null ? '00:00' : formatMinutes(toMinutes(from ?? '00:00')),
    to: toMinutes(to ?? '00:00') === null ? '00:00' : formatMinutes(toMinutes(to ?? '00:00')),
    action: screenOff ? 'off' : `layout:${String(layout ?? '').trim()}`,
    extra,
  };
}

function ruleToConfig(rule) {
  const entry = { ...rule.extra, days: formatDays(rule.days), from: rule.from, to: rule.to };
  if (rule.action === 'off') entry.screen = 'off';
  else entry.layout = rule.action.slice('layout:'.length);
  return entry;
}

function matches(rule, day, minute) {
  const from = toMinutes(rule.from);
  const to = toMinutes(rule.to);
  if (from === null || to === null) return false;
  if (from === to) return rule.days.has(day);
  if (from < to) return rule.days.has(day) && minute >= from && minute < to;
  const yesterday = day === 1 ? 7 : day - 1;
  return (rule.days.has(day) && minute >= from) || (rule.days.has(yesterday) && minute < to);
}

// What the kiosk would do at that moment: the last matching rule wins
function stateAt(rules, day, minute) {
  let state = { kind: 'none', layout: '' };
  for (const rule of rules) {
    if (!isComplete(rule) || !matches(rule, day, minute)) continue;
    state = rule.action === 'off' ? { kind: 'off', layout: '' } : { kind: 'layout', layout: rule.action.slice(7) };
  }
  return state;
}

function isComplete(rule) {
  return rule.days.size > 0 && toMinutes(rule.from) !== null && toMinutes(rule.to) !== null
    && (rule.action === 'off' || rule.action.length > 'layout:'.length);
}

function timeNote(rule) {
  const from = toMinutes(rule.from);
  const to = toMinutes(rule.to);
  if (from === null || to === null) return '';
  if (from === to) return 'ganzer Tag';
  if (to < from) return 'bis zum nächsten Tag';
  return '';
}

export default {
  title: 'Zeitplan',

  async mount(root) {
    let config = {};
    let rules = [];
    let saved = '';
    let nowTimer = null;

    const header = el('div', { className: 'page-header' }, [
      el('h1', { textContent: 'Zeitplan' }),
    ]);
    const content = el('div', { className: 'stack' }, [el('p', { className: 'muted', textContent: 'Lädt …' })]);
    root.append(header, content);

    try {
      config = (await get('/api/config')) || {};
    } catch (error) {
      content.replaceChildren(el('p', { className: 'error', textContent: `Zeitplan nicht ladbar: ${error.message}` }));
      return undefined;
    }
    rules = (Array.isArray(config.schedule) ? config.schedule : []).map(ruleFromConfig);
    saved = serialize();

    const nowLine = el('p', { className: 'sched-now' });
    const list = el('ol', { className: 'sched-list' });
    const preview = el('div', { className: 'sched-week', attrs: { role: 'img', 'aria-label': 'Wochenübersicht des Zeitplans' } });
    const legend = el('div', { className: 'row sched-legend' });
    const errorText = el('p', { className: 'error', hidden: true });
    const dirtyText = el('span', { className: 'sched-dirty', textContent: 'Ungespeicherte Änderungen', hidden: true });
    const saveButton = el('button', { className: 'primary', textContent: 'Speichern', disabled: true });
    const discardButton = el('button', { className: 'secondary', textContent: 'Verwerfen', disabled: true });
    const addButton = el('button', { textContent: '+ Regel' });
    const templateButton = el('button', { className: 'secondary', textContent: 'Vorlage …' });

    content.replaceChildren(
      el('section', { className: 'card stack' }, [
        el('p', { className: 'muted small sched-intro', textContent:
          'Regeln gelten der Reihe nach; spätere Regeln haben Vorrang, wo sie sich überschneiden. '
          + 'Wo keine Regel greift, zeigt die Anzeige das aktive Layout.' }),
        nowLine,
        list,
        el('div', { className: 'row' }, [addButton, templateButton]),
      ]),
      el('section', { className: 'card stack' }, [
        el('h2', { textContent: 'Wochenübersicht' }),
        preview,
        legend,
      ]),
      el('div', { className: 'card row sched-footer' }, [dirtyText, errorText, el('span', { className: 'spacer' }), discardButton, saveButton]),
    );

    // State

    function serialize() {
      return JSON.stringify(rules.map(ruleToConfig));
    }

    function isDirty() {
      return serialize() !== saved;
    }

    function layouts() {
      return Array.isArray(config.layouts) ? config.layouts.filter((layout) => layout && layout.id) : [];
    }

    function layoutName(id) {
      const layout = layouts().find((item) => item.id === id);
      return layout ? (layout.name || layout.id) : id;
    }

    function defaultLayoutId() {
      const ids = layouts().map((layout) => layout.id);
      return ids.includes(config.activeLayout) ? config.activeLayout : (ids[0] || '');
    }

    function changed() {
      const dirty = isDirty();
      dirtyText.hidden = !dirty;
      saveButton.disabled = !dirty;
      discardButton.disabled = !dirty;
      renderPreview();
      renderNow();
    }

    // Rule list

    function renderList() {
      if (!rules.length) {
        list.replaceChildren(el('li', { className: 'empty sched-empty', textContent: 'Keine Regeln – die Anzeige zeigt immer das aktive Layout.' }));
        return;
      }
      list.replaceChildren(...rules.map(renderRule));
    }

    function renderRule(rule, index) {
      const dayButtons = DAY_LABELS.map((label, i) => {
        const day = i + 1;
        const button = el('button', {
          type: 'button',
          className: 'small sched-day',
          textContent: label,
          attrs: { 'aria-pressed': String(rule.days.has(day)), 'aria-label': `${label} ${rule.days.has(day) ? 'an' : 'aus'}` },
        });
        button.addEventListener('click', () => {
          if (rule.days.has(day)) rule.days.delete(day);
          else rule.days.add(day);
          rule.daysInvalid = false;
          refreshRule();
        });
        return button;
      });

      const from = el('input', { type: 'time', value: rule.from, step: 60, required: true });
      const to = el('input', { type: 'time', value: rule.to, step: 60, required: true });
      const note = el('span', { className: 'muted small sched-note', textContent: timeNote(rule) });
      from.addEventListener('change', () => { rule.from = from.value || '00:00'; refreshRule(); });
      to.addEventListener('change', () => { rule.to = to.value || '00:00'; refreshRule(); });

      const actionSelect = el('select', { attrs: { 'aria-label': 'Aktion' } }, actionOptions(rule));
      actionSelect.addEventListener('change', () => { rule.action = actionSelect.value; refreshRule(); });

      const warning = el('p', { className: 'error small sched-warning', hidden: true });

      const item = el('li', { className: `sched-rule ${rule.action === 'off' ? 'is-off' : 'is-layout'}` }, [
        el('div', { className: 'sched-rule-head' }, [
          el('strong', { className: 'sched-rule-number', textContent: `${index + 1}.` }),
          el('div', { className: 'sched-days', attrs: { role: 'group', 'aria-label': 'Tage' } }, dayButtons),
          el('span', { className: 'spacer' }),
          iconButton('↑', 'Nach oben', index === 0, () => moveRule(index, -1)),
          iconButton('↓', 'Nach unten', index === rules.length - 1, () => moveRule(index, 1)),
          iconButton('⧉', 'Duplizieren', false, () => duplicateRule(index)),
          iconButton('✕', 'Löschen', false, () => removeRule(index)),
        ]),
        el('div', { className: 'sched-rule-body' }, [
          el('label', { className: 'inline' }, ['von', from]),
          el('label', { className: 'inline' }, ['bis', to]),
          note,
          el('span', { className: 'spacer' }),
          actionSelect,
        ]),
        warning,
      ]);

      function refreshRule() {
        note.textContent = timeNote(rule);
        item.className = `sched-rule ${rule.action === 'off' ? 'is-off' : 'is-layout'}`;
        dayButtons.forEach((button, i) => {
          const on = rule.days.has(i + 1);
          button.setAttribute('aria-pressed', String(on));
          button.setAttribute('aria-label', `${DAY_LABELS[i]} ${on ? 'an' : 'aus'}`);
        });
        const problems = ruleProblems(rule);
        warning.textContent = problems.join(' · ');
        warning.hidden = problems.length === 0;
        changed();
      }

      const problems = ruleProblems(rule);
      warning.textContent = problems.join(' · ');
      warning.hidden = problems.length === 0;
      return item;
    }

    function actionOptions(rule) {
      const options = layouts().map((layout) =>
        el('option', { value: `layout:${layout.id}`, textContent: `Layout zeigen: ${layout.name || layout.id}` }));
      const current = rule.action.slice('layout:'.length);
      if (rule.action !== 'off' && current && !layouts().some((layout) => layout.id === current)) {
        options.unshift(el('option', { value: rule.action, textContent: `Layout zeigen: ${current} (unbekannt)` }));
      }
      if (rule.action !== 'off' && !current) {
        options.unshift(el('option', { value: 'layout:', textContent: 'Layout wählen …', disabled: true }));
      }
      options.push(el('option', { value: 'off', textContent: 'Bildschirm aus' }));
      for (const option of options) option.selected = option.value === rule.action;
      return options;
    }

    function ruleProblems(rule) {
      const problems = [];
      if (rule.daysInvalid) problems.push('Die gespeicherten Tage waren unlesbar – bitte neu wählen');
      else if (!rule.days.size) problems.push('Mindestens einen Tag wählen');
      if (toMinutes(rule.from) === null || toMinutes(rule.to) === null) problems.push('Uhrzeit fehlt');
      if (rule.action !== 'off') {
        const id = rule.action.slice('layout:'.length);
        if (!id) problems.push('Layout wählen');
        else if (!layouts().some((layout) => layout.id === id)) problems.push(`Layout „${id}“ gibt es nicht`);
      }
      return problems;
    }

    function iconButton(text, label, disabled, onClick) {
      const button = el('button', { type: 'button', className: 'icon-button small', textContent: text, title: label, disabled, attrs: { 'aria-label': label } });
      button.addEventListener('click', onClick);
      return button;
    }

    function newRule() {
      const id = defaultLayoutId();
      return { days: new Set([1, 2, 3, 4, 5, 6, 7]), daysInvalid: false, from: '07:00', to: '22:00', action: id ? `layout:${id}` : 'off', extra: {} };
    }

    function copyRule(rule) {
      return { ...rule, days: new Set(rule.days), extra: { ...rule.extra } };
    }

    function moveRule(index, offset) {
      const target = index + offset;
      if (target < 0 || target >= rules.length) return;
      [rules[index], rules[target]] = [rules[target], rules[index]];
      renderList();
      changed();
    }

    function duplicateRule(index) {
      rules.splice(index + 1, 0, copyRule(rules[index]));
      renderList();
      changed();
    }

    function removeRule(index) {
      rules.splice(index, 1);
      renderList();
      changed();
    }

    // Templates

    const TEMPLATES = [
      {
        label: 'Nachts aus (22:30–07:00)',
        rules: () => [{ days: new Set([1, 2, 3, 4, 5, 6, 7]), daysInvalid: false, from: '22:30', to: '07:00', action: 'off', extra: {} }],
      },
      {
        label: 'Werktags Öffnungszeiten (Mo–Fr 09:00–18:00), sonst aus',
        needsLayout: true,
        rules: () => [
          { days: new Set([1, 2, 3, 4, 5, 6, 7]), daysInvalid: false, from: '00:00', to: '00:00', action: 'off', extra: {} },
          { days: new Set([1, 2, 3, 4, 5]), daysInvalid: false, from: '09:00', to: '18:00', action: `layout:${defaultLayoutId()}`, extra: {} },
        ],
      },
    ];

    async function applyTemplate() {
      const choice = await dialog({
        title: 'Vorlage einfügen',
        body: el('p', { className: 'muted', textContent: rules.length
          ? 'Die Vorlage wird an die bestehenden Regeln angehängt und hat damit Vorrang.'
          : 'Die Vorlage legt passende Regeln an.' }),
        buttons: [
          { label: 'Abbrechen', value: -1 },
          ...TEMPLATES.map((template, index) => ({ label: template.label, value: index, kind: index === 0 ? 'primary' : 'secondary' })),
        ],
      });
      if (choice === null || choice === -1) return;
      const template = TEMPLATES[choice];
      if (template.needsLayout && !defaultLayoutId()) {
        toast('Für diese Vorlage zuerst ein Layout anlegen', 'error');
        return;
      }
      rules.push(...template.rules());
      renderList();
      changed();
    }

    // Week preview

    function segmentsForDay(day) {
      const segments = [];
      for (let minute = 0; minute < MINUTES_PER_DAY; minute += PREVIEW_STEP) {
        const state = stateAt(rules, day, minute);
        const key = `${state.kind}:${state.layout}`;
        const last = segments[segments.length - 1];
        if (last && last.key === key) last.end = minute + PREVIEW_STEP;
        else segments.push({ key, state, start: minute, end: minute + PREVIEW_STEP });
      }
      return segments;
    }

    function colorClass(state, layoutIds) {
      if (state.kind === 'off') return 'sched-off';
      if (state.kind === 'none') return 'sched-none';
      const index = layoutIds.indexOf(state.layout);
      return `sched-c${(index < 0 ? layoutIds.length : index) % 6}`;
    }

    function describe(state) {
      if (state.kind === 'off') return 'Bildschirm aus';
      if (state.kind === 'none') return 'aktives Layout';
      return `Layout „${layoutName(state.layout)}“`;
    }

    function renderPreview() {
      const used = [];
      const rows = [];
      const scheduledLayouts = rules.filter((rule) => rule.action !== 'off').map((rule) => rule.action.slice(7)).filter(Boolean);
      const layoutIds = [...new Set(scheduledLayouts)];
      for (let day = 1; day <= 7; day++) {
        const bar = el('div', { className: 'sched-bar' });
        for (const segment of segmentsForDay(day)) {
          const block = el('span', {
            className: `sched-segment ${colorClass(segment.state, layoutIds)}`,
            title: `${DAY_LABELS[day - 1]} ${formatMinutes(segment.start)}–${formatMinutes(segment.end % MINUTES_PER_DAY)}: ${describe(segment.state)}`,
          });
          block.style.left = `${(segment.start / MINUTES_PER_DAY) * 100}%`;
          block.style.width = `${((segment.end - segment.start) / MINUTES_PER_DAY) * 100}%`;
          bar.append(block);
          if (!used.includes(segment.key)) used.push(segment.key);
        }
        rows.push(el('div', { className: 'sched-row' }, [el('span', { className: 'sched-row-label', textContent: DAY_LABELS[day - 1] }), bar]));
      }
      const hours = el('div', { className: 'sched-row sched-hours', attrs: { 'aria-hidden': 'true' } }, [
        el('span', { className: 'sched-row-label' }),
        el('div', { className: 'sched-hour-scale' }, [0, 6, 12, 18, 24].map((hour) => {
          const mark = el('span', { textContent: pad(hour) });
          mark.style.left = `${(hour / 24) * 100}%`;
          return mark;
        })),
      ]);
      preview.replaceChildren(...rows, hours);

      legend.replaceChildren(...used.map((key) => {
        const [kind, layout] = [key.slice(0, key.indexOf(':')), key.slice(key.indexOf(':') + 1)];
        const state = { kind, layout };
        return el('span', { className: 'sched-legend-item' }, [
          el('span', { className: `sched-swatch ${colorClass(state, layoutIds)}` }),
          describe(state),
        ]);
      }));
    }

    function renderNow() {
      const now = new Date();
      const day = ((now.getDay() + 6) % 7) + 1;
      const minute = now.getHours() * 60 + now.getMinutes();
      const state = stateAt(rules, day, minute);
      nowLine.replaceChildren(
        el('strong', { textContent: 'Jetzt: ' }),
        el('span', { textContent: describe(state) }),
        el('span', { className: 'muted small', textContent: ` (${DAY_LABELS[day - 1]} ${formatMinutes(minute)} nach der Uhr dieses Geräts – der Kiosk nutzt seine eigene)` }),
      );
    }

    // Saving

    async function save() {
      const incomplete = rules.findIndex((rule) => ruleProblems(rule).length > 0);
      if (incomplete >= 0) {
        errorText.textContent = `Regel ${incomplete + 1}: ${ruleProblems(rules[incomplete]).join(', ')}`;
        errorText.hidden = false;
        return;
      }
      errorText.hidden = true;
      saveButton.disabled = true;
      try {
        const current = (await get('/api/config')) || {};
        const schedule = rules.map(ruleToConfig);
        if (schedule.length) current.schedule = schedule;
        else delete current.schedule;
        await put('/api/config', current);
        config = current;
        saved = serialize();
        toast('Zeitplan gespeichert');
        changed();
      } catch (error) {
        errorText.textContent = error.message;
        errorText.hidden = false;
        saveButton.disabled = false;
      }
    }

    async function discard() {
      if (!(await confirm('Änderungen verwerfen?', 'Der zuletzt gespeicherte Zeitplan wird wiederhergestellt.', { label: 'Verwerfen', danger: true }))) return;
      rules = JSON.parse(saved).map(ruleFromConfig);
      renderList();
      changed();
    }

    function onBeforeUnload(event) {
      if (!isDirty()) return;
      event.preventDefault();
      event.returnValue = '';
    }

    addButton.addEventListener('click', () => {
      rules.push(newRule());
      renderList();
      changed();
    });
    templateButton.addEventListener('click', applyTemplate);
    saveButton.addEventListener('click', save);
    discardButton.addEventListener('click', discard);
    window.addEventListener('beforeunload', onBeforeUnload);
    nowTimer = setInterval(renderNow, 30000);

    renderList();
    changed();

    return () => {
      clearInterval(nowTimer);
      window.removeEventListener('beforeunload', onBeforeUnload);
    };
  },
};
