// Layouts view: the list of layouts, the grid editor for the selected one,
// the global quote list, saving (read-modify-write of config.json) and a
// preview on the kiosk.

import { get, post, put } from './api.js';
import { confirm, dialog, el, toast } from './ui.js';
import { createLayoutEditor } from './layout-editor.js';
import { createQuotesEditor } from './layout-quotes.js';
import { TEMPLATES, clone, slugify, uniqueId, validateLayout } from './layout-model.js';

const PREVIEW_SECONDS = 60;

// Unsaved work survives leaving the view, as long as the server state is the same
let draft = null;

const nameOf = (layout) => layout.name || layout.id;

export default {
  title: 'Layouts',

  async mount(root) {
    let config;
    let albums = [];
    let aspect = 16 / 9;
    try {
      [config, albums] = await Promise.all([get('/api/config'), get('/api/albums')]);
      const status = await get('/api/status').catch(() => null);
      const [w, h] = status?.display?.screenSize ?? [];
      if (w > 0 && h > 0) aspect = w / h; // already rotated like the canvas
    } catch (error) {
      root.append(el('p', { className: 'error', textContent: `Layouts nicht ladbar: ${error.message}` }));
      return undefined;
    }

    const serverState = () => ({
      layouts: clone(config.layouts ?? []),
      activeId: config.activeLayout ?? config.layouts?.[0]?.id ?? '',
      quotes: clone(config.quotes ?? []),
    });
    let saved = JSON.stringify(serverState());
    let state = serverState();
    let selectedId = state.activeId;
    let restored = false;
    if (draft && draft.base === saved) {
      state = draft.state;
      selectedId = draft.selectedId;
      restored = true;
    }
    draft = null;

    let editor = null;
    const list = el('ul', { className: 'lay-list' });
    const editorHost = el('div', { className: 'lay-editor-host' });
    const dirtyText = el('span', { className: 'muted small' });
    const saveButton = el('button', { type: 'button', className: 'primary', textContent: 'Speichern', onclick: () => save() });
    const discardButton = el('button', { type: 'button', className: 'secondary', textContent: 'Verwerfen', onclick: discard });
    const previewButton = el('button', { type: 'button', textContent: 'Auf Kiosk vorschauen', onclick: preview });
    const quotesEditor = createQuotesEditor(state.quotes, (quotes) => { state.quotes = quotes; refreshDirty(); });
    const tabs = {
      layouts: el('button', { type: 'button', className: 'small', textContent: 'Layouts', onclick: () => showTab('layouts') }),
      quotes: el('button', { type: 'button', className: 'small', textContent: 'Zitate', onclick: () => showTab('quotes') }),
    };
    const layoutsSection = el('div', { className: 'lay-main' }, [
      el('section', { className: 'card lay-sidebar' }, [
        el('div', { className: 'row' }, [
          el('h2', { className: 'spacer', textContent: 'Layouts' }),
          el('button', { type: 'button', className: 'small', textContent: '+ Neu', onclick: createLayout }),
        ]),
        list,
      ]),
      editorHost,
    ]);

    root.append(
      el('div', { className: 'page-header' }, [
        el('h1', { textContent: 'Layouts' }),
        el('div', { className: 'lay-tabs', attrs: { role: 'tablist' } }, [tabs.layouts, tabs.quotes]),
      ]),
      el('p', { className: 'badge accent lay-restored', hidden: !restored, textContent: 'Nicht gespeicherte Änderungen wiederhergestellt' }),
      layoutsSection,
      quotesEditor.element,
      el('div', { className: 'lay-footer card' }, [dirtyText, el('span', { className: 'spacer' }), discardButton, previewButton, saveButton]),
    );

    const onBeforeUnload = (event) => {
      if (!isDirty()) return;
      event.preventDefault();
      event.returnValue = '';
    };
    window.addEventListener('beforeunload', onBeforeUnload);

    showTab('layouts');
    renderList();
    openEditor();
    refreshDirty();

    // State

    function isDirty() {
      return JSON.stringify(state) !== saved;
    }

    function refreshDirty() {
      const dirty = isDirty();
      dirtyText.textContent = dirty ? 'Ungespeicherte Änderungen' : 'Alles gespeichert';
      dirtyText.classList.toggle('lay-dirty', dirty);
      saveButton.disabled = !dirty;
      discardButton.disabled = !dirty;
      previewButton.disabled = !selectedLayout();
    }

    function selectedLayout() {
      return state.layouts.find((layout) => layout.id === selectedId);
    }

    function showTab(name) {
      layoutsSection.hidden = name !== 'layouts';
      quotesEditor.element.hidden = name !== 'quotes';
      previewButton.hidden = name !== 'layouts';
      for (const [key, button] of Object.entries(tabs)) {
        button.classList.toggle('primary', key === name);
        button.setAttribute('aria-selected', String(key === name));
      }
    }

    // Layout list

    function renderList() {
      if (!state.layouts.length) {
        list.replaceChildren(el('li', { className: 'empty', textContent: 'Noch keine Layouts. Die Anzeige zeigt eine Vollbild-Diashow.' }));
        return;
      }
      list.replaceChildren(...state.layouts.map((layout) => {
        const active = layout.id === state.activeId;
        const item = el('li', { className: `lay-list-item${layout.id === selectedId ? ' selected' : ''}` }, [
          el('button', { type: 'button', className: 'lay-list-select', onclick: () => selectLayout(layout.id) }, [
            el('strong', { textContent: nameOf(layout) }),
            el('span', { className: 'muted small', textContent: `${layout.regions?.length ?? 0} Regionen · ${layout.id}` }),
          ]),
          active ? el('span', { className: 'badge accent', textContent: 'aktiv' }) : null,
        ]);
        if (layout.id === selectedId) {
          item.append(el('div', { className: 'lay-list-actions' }, [
            active ? null : el('button', { type: 'button', className: 'small', textContent: 'Aktivieren', onclick: activate }),
            el('button', { type: 'button', className: 'small secondary', textContent: 'Duplizieren', onclick: duplicate }),
            el('button', { type: 'button', className: 'small secondary', textContent: 'Umbenennen', onclick: rename }),
            active ? null : el('button', { type: 'button', className: 'small secondary', textContent: 'Löschen', onclick: removeLayout }),
          ].filter(Boolean)));
        }
        return item;
      }));
    }

    function selectLayout(id) {
      if (id === selectedId) return;
      selectedId = id;
      renderList();
      openEditor();
      refreshDirty();
    }

    function openEditor() {
      editor?.destroy();
      editor = null;
      const layout = selectedLayout();
      if (!layout) {
        editorHost.replaceChildren(el('section', { className: 'card empty' }, [
          el('p', { textContent: 'Wähle links ein Layout aus oder lege ein neues an.' }),
          el('button', { type: 'button', className: 'primary', textContent: 'Neues Layout', onclick: createLayout }),
        ]));
        return;
      }
      editor = createLayoutEditor({
        layout,
        albums,
        aspect,
        onChange(next) {
          const index = state.layouts.findIndex((item) => item.id === selectedId);
          if (index >= 0) state.layouts[index] = next;
          refreshDirty();
          renderList();
        },
      });
      editorHost.replaceChildren(editor.element);
    }

    async function askLayout() {
      const name = el('input', { value: '', placeholder: 'z. B. Abend', autocomplete: 'off', required: true });
      const template = el('select', {}, TEMPLATES.map((item) => el('option', { value: item.key, textContent: item.label, selected: item.key === 'full-widgets' })));
      setTimeout(() => name.focus());
      const ok = await dialog({
        title: 'Neues Layout',
        body: el('div', { className: 'stack' }, [el('label', {}, ['Name', name]), el('label', {}, ['Vorlage', template])]),
        buttons: [{ label: 'Abbrechen', value: false }, { label: 'Anlegen', value: true, kind: 'primary' }],
      });
      if (!ok || !name.value.trim()) return null;
      return { name: name.value.trim(), template: TEMPLATES.find((item) => item.key === template.value) };
    }

    async function createLayout() {
      const answer = await askLayout();
      if (!answer) return;
      const id = uniqueId(slugify(answer.name), state.layouts.map((layout) => layout.id));
      state.layouts.push({ id, name: answer.name, ...answer.template.make() });
      if (!state.activeId) state.activeId = id;
      selectedId = id;
      afterStructureChange();
    }

    function duplicate() {
      const layout = selectedLayout();
      if (!layout) return;
      const copy = clone(layout);
      copy.id = uniqueId(`${layout.id}-kopie`, state.layouts.map((item) => item.id));
      copy.name = `${nameOf(layout)} (Kopie)`;
      state.layouts.push(copy);
      selectedId = copy.id;
      afterStructureChange();
    }

    async function rename() {
      const layout = selectedLayout();
      if (!layout) return;
      const input = el('input', { value: nameOf(layout), autocomplete: 'off' });
      setTimeout(() => input.select());
      const ok = await dialog({
        title: 'Layout umbenennen',
        body: el('div', { className: 'stack' }, [
          el('label', {}, ['Name', input]),
          el('p', { className: 'muted small', textContent: `Die ID „${layout.id}“ bleibt gleich, damit Zeitpläne weiter passen.` }),
        ]),
        buttons: [{ label: 'Abbrechen', value: false }, { label: 'Umbenennen', value: true, kind: 'primary' }],
      });
      if (!ok || !input.value.trim()) return;
      layout.name = input.value.trim();
      afterStructureChange(false);
    }

    async function removeLayout() {
      const layout = selectedLayout();
      if (!layout || layout.id === state.activeId) return;
      if (!(await confirm('Layout löschen?', `„${nameOf(layout)}“ wird beim Speichern entfernt.`, { label: 'Löschen', danger: true }))) return;
      state.layouts = state.layouts.filter((item) => item !== layout);
      selectedId = state.activeId || state.layouts[0]?.id || '';
      afterStructureChange();
    }

    async function activate() {
      const layout = selectedLayout();
      if (!layout) return;
      state.activeId = layout.id;
      renderList();
      if (await save('Aktiviert und gespeichert')) toast(`„${nameOf(layout)}“ läuft jetzt auf dem Kiosk.`);
    }

    function afterStructureChange(reopen = true) {
      renderList();
      if (reopen) openEditor();
      refreshDirty();
    }

    // Saving

    async function save(message = 'Gespeichert') {
      const problems = state.layouts.flatMap((layout) => validateLayout(layout).map((problem) => `${nameOf(layout)}: ${problem}`));
      if (problems.length) {
        await dialog({
          title: 'Noch nicht speicherbar',
          body: el('ul', { className: 'lay-problems' }, problems.map((problem) => el('li', { textContent: problem }))),
        });
        return false;
      }
      saveButton.disabled = true;
      try {
        const fresh = await get('/api/config');
        fresh.layouts = clone(state.layouts);
        if (state.activeId && state.layouts.some((layout) => layout.id === state.activeId)) fresh.activeLayout = state.activeId;
        else delete fresh.activeLayout;
        fresh.quotes = clone(state.quotes);
        await put('/api/config', fresh);
        config = fresh;
        saved = JSON.stringify(state);
        toast(message);
        return true;
      } catch (error) {
        toast(`Speichern fehlgeschlagen: ${error.message}`, 'error');
        return false;
      } finally {
        refreshDirty();
      }
    }

    async function discard() {
      if (!(await confirm('Änderungen verwerfen?', 'Alles seit dem letzten Speichern geht verloren.', { label: 'Verwerfen', danger: true }))) return;
      state = JSON.parse(saved);
      if (!selectedLayout()) selectedId = state.activeId || state.layouts[0]?.id || '';
      quotesEditor.setQuotes(state.quotes);
      afterStructureChange();
    }

    // Preview on the kiosk, without saving

    async function preview() {
      const layout = editor?.layout();
      if (!layout) return;
      const problems = validateLayout(layout);
      if (problems.length) {
        toast(problems[0], 'error');
        return;
      }
      previewButton.disabled = true;
      try {
        await post('/api/display/preview', { layout, seconds: PREVIEW_SECONDS });
        toast(`Vorschau läuft ${PREVIEW_SECONDS} s auf dem Kiosk`);
        await new Promise((resolve) => setTimeout(resolve, 3000));
        await showScreenshot();
      } catch (error) {
        toast(error.status === 503 ? 'Anzeige nicht verbunden – Vorschau nicht möglich.' : error.message, 'error');
      } finally {
        refreshDirty();
      }
    }

    async function showScreenshot() {
      for (;;) {
        const image = el('img', { className: 'lay-shot', alt: 'Bildschirmfoto der Vorschau', src: `/api/display/screenshot?maxWidth=1280&t=${Date.now()}` });
        image.addEventListener('error', () => image.replaceWith(el('p', { className: 'error', textContent: 'Kein Bildschirmfoto erhalten.' })));
        const result = await dialog({
          title: 'Vorschau auf dem Kiosk',
          body: image,
          wide: true,
          buttons: [{ label: 'Schließen', value: 'close' }, { label: 'Erneut aufnehmen', value: 'again', kind: 'primary' }],
        });
        if (result !== 'again') return;
      }
    }

    return () => {
      window.removeEventListener('beforeunload', onBeforeUnload);
      editor?.destroy();
      if (isDirty()) draft = { base: saved, state: clone(state), selectedId };
    };
  },
};
