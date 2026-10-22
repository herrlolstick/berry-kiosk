// Albums view: the folders of the media library as a tree, with create,
// rename/move and delete (to the server's trash).

import { del, get, patch, post, thumbUrl, withQuery } from './api.js';
import { albumLabel, confirm, dialog, el, thumbnail, toast } from './ui.js';
import { cleanAlbumName } from './view-upload.js';

const depthOf = (name) => (name ? name.split('/').length : 0);
const leafName = (name) => name.split('/').pop();

function countText(album) {
  const hidden = album.count - (album.enabled ?? album.count);
  const pictures = album.count === 1 ? '1 Bild' : `${album.count} Bilder`;
  return hidden > 0 ? `${pictures}, ${hidden} ausgeblendet` : pictures;
}

// Dialog with one text field. Resolves with the trimmed text or null.
async function askName(title, label, value = '', confirmLabel = 'Speichern') {
  const input = el('input', { value, autocomplete: 'off', placeholder: 'z. B. reisen/norwegen' });
  const body = el('div', { className: 'stack' }, [
    el('label', {}, [label, input]),
    el('p', { className: 'muted small', textContent: 'Mit „/“ entstehen Unteralben, z. B. „reisen/norwegen“.' }),
  ]);
  setTimeout(() => input.select());
  const ok = await dialog({
    title,
    body,
    buttons: [{ label: 'Abbrechen', value: false }, { label: confirmLabel, value: true, kind: 'primary' }],
  });
  return ok ? input.value.trim() : null;
}

export default {
  title: 'Alben',

  async mount(root) {
    const list = el('ul', { className: 'alb-list' });
    const status = el('p', { className: 'muted', textContent: 'Lädt …' });
    let disposed = false;

    root.append(
      el('div', { className: 'page-header' }, [
        el('h1', { textContent: 'Alben' }),
        el('button', { type: 'button', className: 'primary', textContent: 'Neues Album', onclick: createAlbum }),
      ]),
      el('section', { className: 'card' }, [status, list]),
    );

    async function load() {
      let albums;
      try {
        albums = await get('/api/albums');
      } catch (error) {
        status.hidden = false;
        status.className = 'error';
        status.textContent = `Alben nicht ladbar: ${error.message}`;
        return;
      }
      if (disposed) return;
      albums.sort((a, b) => a.name.localeCompare(b.name, 'de', { sensitivity: 'base' }));
      const real = albums.filter((album) => album.name !== '' || album.count > 0);
      status.hidden = real.length > 0;
      status.className = 'empty';
      status.textContent = 'Noch keine Alben. Lege eines an oder lade Bilder hoch.';
      list.replaceChildren(...albums.map(renderAlbum));
    }

    function renderAlbum(album) {
      const cover = el('div', { className: 'alb-cover' }, [
        album.cover ? thumbnail(thumbUrl(album.cover), '') : el('div', { className: 'thumb-placeholder' }),
      ]);
      const actions = [
        el('a', { className: 'button small', href: `#/mediathek?album=${encodeURIComponent(album.name)}`, textContent: 'Ansehen' }),
        el('a', { className: 'button small secondary', href: `#/hochladen?album=${encodeURIComponent(album.name)}`, textContent: 'Hochladen' }),
      ];
      if (album.name) {
        actions.push(
          el('button', { type: 'button', className: 'small secondary', textContent: 'Umbenennen', onclick: () => renameAlbum(album) }),
          el('button', { type: 'button', className: 'small secondary alb-delete', textContent: 'Löschen', onclick: () => deleteAlbum(album) }),
        );
      }
      const item = el('li', { className: 'alb-item', dataset: { depth: Math.min(depthOf(album.name), 6) } }, [
        cover,
        el('div', { className: 'alb-info' }, [
          el('a', {
            className: 'alb-name',
            href: `#/mediathek?album=${encodeURIComponent(album.name)}`,
            textContent: album.name ? leafName(album.name) : albumLabel(''),
            title: albumLabel(album.name),
          }),
          el('span', { className: 'muted small', textContent: countText(album) }),
        ]),
        el('div', { className: 'alb-actions row' }, actions),
      ]);
      return item;
    }

    async function createAlbum() {
      const raw = await askName('Neues Album', 'Name', '', 'Anlegen');
      if (raw === null) return;
      try {
        const name = cleanAlbumName(raw);
        if (!name) throw new Error('Bitte einen Namen eingeben');
        const created = await post('/api/albums', { name });
        toast(`Album „${albumLabel(created.name)}“ angelegt`);
        await load();
      } catch (error) {
        toast(error.message, 'error');
      }
    }

    async function renameAlbum(album) {
      const raw = await askName('Album umbenennen', 'Neuer Name oder Pfad', album.name);
      if (raw === null) return;
      try {
        const to = cleanAlbumName(raw);
        if (!to) throw new Error('Bitte einen Namen eingeben');
        if (to === album.name) return;
        const result = await patch('/api/albums', { from: album.name, to });
        toast(`„${albumLabel(album.name)}“ heißt jetzt „${albumLabel(result?.name ?? to)}“`);
        await load();
      } catch (error) {
        toast(error.status === 409 ? `Ein Album „${raw}“ gibt es schon` : error.message, 'error');
      }
    }

    async function deleteAlbum(album) {
      const what = album.count ? `samt ${album.count === 1 ? 'einem Bild' : `${album.count} Bildern`} ` : '';
      const ok = await confirm(
        'Album löschen?',
        `Der Ordner „${album.name}“ wird ${what}in den Papierkorb auf dem Gerät verschoben. Von dort lässt er sich von Hand zurückholen.`,
        { label: 'In den Papierkorb', danger: true },
      );
      if (!ok) return;
      try {
        await del(withQuery('/api/albums', { name: album.name }));
        toast(`Album „${album.name}“ ist im Papierkorb`);
        await load();
      } catch (error) {
        toast(error.message, 'error');
      }
    }

    await load();

    return () => {
      disposed = true;
    };
  },
};
