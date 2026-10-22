// Übersicht: live screenshot and remote control of the display, library and
// storage state, newest pictures.

import { get, post, thumbUrl, withQuery } from './api.js';
import { el, clear, toast, formatBytes, formatDate, thumbnail } from './ui.js';

const STATUS_INTERVAL = 10000;
const SCREENSHOT_INTERVAL = 15000;

function formatUptime(seconds) {
  if (!Number.isFinite(seconds)) return '–';
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (days) return `${days} T. ${hours} Std.`;
  if (hours) return `${hours} Std. ${minutes} Min.`;
  return `${minutes} Min.`;
}

function formatAgo(timestamp) {
  if (!timestamp) return '';
  const seconds = Math.max(0, Math.round((Date.now() - timestamp) / 1000));
  if (seconds < 60) return `vor ${seconds} s`;
  return `vor ${Math.round(seconds / 60)} Min.`;
}

function card(title, ...children) {
  return el('section', { className: 'card' }, [el('h2', { textContent: title }), ...children]);
}

function thumbLink(media, label) {
  return el('a', { href: `#/mediathek/${media.id}`, className: 'dash-thumb', title: label }, [
    thumbnail(thumbUrl(media), label),
    el('span', { className: 'dash-thumb-label', textContent: label }),
  ]);
}

export default {
  title: 'Übersicht',

  mount(root) {
    const timers = [];
    const timeouts = [];
    const later = (fn, ms) => timeouts.push(setTimeout(fn, ms));
    let status = null;
    let screenshotAt = 0;
    let busy = false;

    // Anzeige
    const screen = el('div', { className: 'dash-screen' });
    const screenImage = el('img', { alt: 'Bildschirmfoto der Anzeige', hidden: true });
    const screenEmpty = el('div', { className: 'dash-screen-empty', textContent: 'Noch kein Bildschirmfoto' });
    screen.append(screenImage, screenEmpty);
    const screenTime = el('span', { className: 'muted small' });
    const refreshButton = el('button', { className: 'secondary small', textContent: 'Aktualisieren', onclick: () => loadScreenshot() });
    const badges = el('div', { className: 'row dash-badges' });
    const hint = el('p', { className: 'muted small', hidden: true, textContent: 'Anzeige nicht verbunden – läuft berry-kiosk-display?' });
    const nextButton = el('button', { className: 'primary', textContent: 'Nächstes Bild', onclick: () => command('next') });
    const pauseButton = el('button', { textContent: 'Pause', onclick: () => command(status?.display?.paused ? 'resume' : 'pause') });

    // Läuft gerade
    const showing = el('div', { className: 'thumb-grid dash-showing' });
    const showingCard = card('Läuft gerade', showing);
    showingCard.hidden = true;

    // Mediathek and storage
    const libraryInfo = el('div', { className: 'stack dash-facts' });
    const syncButton = el('button', { className: 'secondary', textContent: 'Neu einlesen', onclick: sync });
    const storageBar = el('span');
    const storageText = el('p', { className: 'muted small' });

    // Newest pictures
    const recent = el('div', { className: 'thumb-grid dash-recent' });
    const footer = el('p', { className: 'muted small dash-footer' });

    root.append(
      el('div', { className: 'page-header' }, [
        el('h1', { textContent: 'Übersicht' }),
        el('a', { className: 'button primary', href: '#/hochladen', textContent: 'Hochladen' }),
      ]),
      el('div', { className: 'dash-grid' }, [
        card('Anzeige',
          screen,
          el('div', { className: 'row dash-screen-bar' }, [screenTime, el('span', { className: 'spacer' }), refreshButton]),
          badges,
          el('div', { className: 'row' }, [nextButton, pauseButton]),
          hint),
        el('div', { className: 'stack' }, [
          showingCard,
          card('Mediathek', libraryInfo, el('div', { className: 'row' }, [syncButton])),
          card('Speicher', el('div', { className: 'progress dash-storage' }, [storageBar]), storageText),
        ]),
      ]),
      el('section', { className: 'card dash-recent-card' }, [
        el('div', { className: 'page-header' }, [
          el('h2', { textContent: 'Zuletzt hinzugefügt' }),
          el('a', { href: '#/mediathek', textContent: 'Alle ansehen' }),
        ]),
        recent,
      ]),
      footer,
    );

    function badge(text, kind = '') {
      return el('span', { className: `badge ${kind}`.trim(), textContent: text });
    }

    function renderDisplay(display = {}) {
      const connected = !!display.connected;
      const items = [badge(connected ? 'verbunden' : 'getrennt', connected ? 'accent' : 'warning')];
      if (display.paused) items.push(badge('pausiert', 'warning'));
      if (display.renderTier) items.push(badge(`Render-Stufe ${display.renderTier}`));
      if (display.layout) items.push(badge(`Layout „${display.layout}“`));
      if (display.screenSize) items.push(badge(`${display.screenSize[0]} × ${display.screenSize[1]}`));
      if (display.device) items.push(badge(display.device));
      badges.replaceChildren(...items);

      hint.hidden = connected;
      nextButton.disabled = !connected || busy;
      pauseButton.disabled = !connected || busy;
      pauseButton.textContent = display.paused ? 'Weiter' : 'Pause';
      refreshButton.disabled = !connected;

      const size = display.screenSize || display.renderSize;
      if (size && size[0] > 0 && size[1] > 0) screen.style.aspectRatio = `${size[0]} / ${size[1]}`;

      const visible = (display.showing || []).filter((item) => item.id);
      showingCard.hidden = visible.length === 0;
      showing.replaceChildren(...visible.map((item) =>
        thumbLink({ id: item.id }, `${item.region}: ${item.path}`)));
    }

    function renderLibrary(library = {}) {
      const pending = library.pendingRenditions || 0;
      const facts = [
        el('div', { className: 'dash-fact' }, [
          el('strong', { textContent: (library.images ?? 0).toLocaleString('de-DE') }),
          el('span', { className: 'muted', textContent: ' Bilder' }),
        ]),
        el('div', {}, [pending > 0
          ? badge(`Varianten werden erstellt: ${pending.toLocaleString('de-DE')}`, 'warning')
          : badge('alles bereit', 'accent')]),
      ];
      if (library.renditions?.length) {
        facts.push(el('div', { className: 'muted small', textContent: `Varianten: ${library.renditions.join(', ')}` }));
      }
      if (library.updated) {
        facts.push(el('div', { className: 'muted small', textContent: `Stand: ${formatDate(library.updated, true)}` }));
      }
      libraryInfo.replaceChildren(...facts);
    }

    function renderStorage(storage = {}) {
      const { free, total } = storage;
      if (!total) {
        storageBar.style.width = '0';
        storageText.textContent = 'Keine Angaben';
        return;
      }
      const used = total - free;
      storageBar.style.width = `${Math.min(100, (used / total) * 100).toFixed(1)}%`;
      storageText.textContent = `${formatBytes(used)} belegt · ${formatBytes(free)} frei von ${formatBytes(total)}`;
    }

    function renderFooter() {
      if (!status) return;
      footer.textContent = `berry-kiosk ${status.version || ''} · läuft seit ${formatUptime(status.uptimeSeconds)}`;
    }

    async function loadStatus() {
      try {
        status = await get('/api/status');
        renderDisplay(status.display);
        renderLibrary(status.library);
        renderStorage(status.storage);
        renderFooter();
      } catch (error) {
        if (error.status !== 401) badges.replaceChildren(badge('Status nicht abrufbar', 'warning'));
      }
    }

    async function loadRecent() {
      try {
        const result = await get(withQuery('/api/media', { limit: 6 }));
        clear(recent);
        if (!result.items.length) {
          recent.append(el('p', { className: 'empty', textContent: 'Noch keine Bilder – lade die ersten hoch.' }));
          return;
        }
        recent.append(...result.items.map((media) => thumbLink(media, media.title || media.path)));
      } catch (error) {
        if (error.status !== 401) recent.replaceChildren(el('p', { className: 'error', textContent: error.message }));
      }
    }

    function loadScreenshot() {
      if (status && !status.display?.connected) return;
      const url = withQuery('/api/display/screenshot', { maxWidth: 1280, t: Date.now() });
      // Load off-screen first, so a failure keeps the last good picture
      const probe = new Image();
      probe.addEventListener('load', () => {
        screenImage.src = probe.src;
        screenImage.hidden = false;
        screenEmpty.hidden = true;
        screenshotAt = Date.now();
        updateScreenTime();
      });
      probe.addEventListener('error', () => {
        if (screenImage.hidden) screenEmpty.textContent = 'Kein Bildschirmfoto – ist die Anzeige verbunden?';
      });
      probe.src = url;
    }

    function updateScreenTime() {
      screenTime.textContent = screenshotAt ? `Aufgenommen ${formatAgo(screenshotAt)}` : '';
    }

    async function command(name) {
      busy = true;
      renderDisplay(status?.display);
      try {
        await post(`/api/display/${name}`);
        // The cross-fade takes a moment; photograph the new picture, not the old one
        if (name === 'next') later(loadScreenshot, 2500);
        await loadStatus();
      } catch (error) {
        toast(error.message, 'error');
      } finally {
        busy = false;
        renderDisplay(status?.display);
      }
    }

    async function sync() {
      syncButton.disabled = true;
      try {
        await post('/api/library/sync');
        toast('Mediathek wird neu eingelesen');
        later(loadStatus, 2000);
      } catch (error) {
        toast(error.message, 'error');
      } finally {
        syncButton.disabled = false;
      }
    }

    function every(ms, fn) {
      timers.push(setInterval(() => {
        if (document.visibilityState === 'visible') fn();
      }, ms));
    }

    function onVisibility() {
      if (document.visibilityState !== 'visible') return;
      loadStatus();
      if (Date.now() - screenshotAt > SCREENSHOT_INTERVAL) loadScreenshot();
    }

    document.addEventListener('visibilitychange', onVisibility);
    every(STATUS_INTERVAL, loadStatus);
    every(SCREENSHOT_INTERVAL, loadScreenshot);
    timers.push(setInterval(updateScreenTime, 1000));

    loadStatus().then(loadScreenshot);
    loadRecent();

    return () => {
      timers.forEach(clearInterval);
      timeouts.forEach(clearTimeout);
      document.removeEventListener('visibilitychange', onVisibility);
    };
  },
};
