// Einstellungen: password, display and library options from config.json.
// Saving reads the current config first and changes only the edited keys,
// so layouts and everything else stay untouched.

import { get, post, put } from './api.js';
import { el, toast } from './ui.js';

const LOCALES = [
  ['de_DE', 'Deutsch'],
  ['en_GB', 'English (UK)'],
  ['en_US', 'English (US)'],
  ['fr_FR', 'Français'],
];

const RENDITIONS = [
  ['auto', 'Automatisch nach Anzeige'],
  ['1080p', 'Nur 1080p'],
  ['2160p', 'Nur 4K'],
  ['both', '1080p und 4K'],
];

function renditionsToChoice(value) {
  if (!Array.isArray(value) || value.length === 0) return 'auto';
  const has1080 = value.includes('1080p');
  const has2160 = value.includes('2160p');
  if (has1080 && has2160) return 'both';
  return has2160 ? '2160p' : has1080 ? '1080p' : 'auto';
}

function choiceToRenditions(choice) {
  return { auto: 'auto', '1080p': ['1080p'], '2160p': ['2160p'], both: ['1080p', '2160p'] }[choice];
}

const POWER = [
  ['none', 'Nur schwarzes Bild'],
  ['cec', 'HDMI-CEC – Fernseher (benötigt v4l-utils)'],
  ['ddc', 'DDC/CI – Monitore (benötigt ddcutil und I²C-Zugriff)'],
];

const PROFILES = [
  ['home', 'Zuhause'],
  ['public', 'Öffentlicher Raum'],
];

function select(name, options, value) {
  return el('select', { name }, options.map(([optionValue, label]) =>
    el('option', { value: String(optionValue), textContent: label, selected: String(optionValue) === String(value) })));
}

function field(label, control, note) {
  return el('label', {}, [label, control, note ? el('span', { className: 'muted small', textContent: note }) : null]);
}

function section(title, description, content) {
  return el('section', { className: 'card set-section' }, [
    el('h2', { textContent: title }),
    description ? el('p', { className: 'muted small', textContent: description }) : null,
    content,
  ]);
}

// A form whose save button lights up when something changed
function settingsForm(fields, onSave) {
  const error = el('p', { className: 'error', hidden: true });
  const dirty = el('span', { className: 'set-dirty', textContent: 'Ungespeicherte Änderungen', hidden: true });
  const button = el('button', { type: 'submit', className: 'primary', textContent: 'Speichern', disabled: true });
  const form = el('form', { className: 'set-form' }, [
    el('div', { className: 'form-grid two' }, fields),
    error,
    el('div', { className: 'row' }, [button, dirty]),
  ]);

  const markDirty = () => {
    dirty.hidden = false;
    button.disabled = false;
  };
  form.addEventListener('input', markDirty);
  form.addEventListener('change', markDirty);
  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    error.hidden = true;
    button.disabled = true;
    try {
      await onSave(form);
      dirty.hidden = true;
      toast('Gespeichert');
    } catch (failure) {
      error.textContent = failure.message;
      error.hidden = false;
      button.disabled = false;
    }
  });
  return form;
}

function passwordSection() {
  const message = el('p', { className: 'error', hidden: true });
  const button = el('button', { type: 'submit', className: 'primary', textContent: 'Passwort ändern' });
  const input = (name, autocomplete) =>
    el('input', { name, type: 'password', autocomplete, required: true });
  const form = el('form', { className: 'set-form' }, [
    el('div', { className: 'form-grid two' }, [
      el('div', { className: 'full' }, [field('Aktuelles Passwort', input('current', 'current-password'))]),
      field('Neues Passwort', input('next', 'new-password'), 'Mindestens 8 Zeichen'),
      field('Wiederholen', input('repeat', 'new-password')),
    ]),
    message,
    el('div', { className: 'row' }, [button]),
  ]);

  const fail = (text) => {
    message.textContent = text;
    message.hidden = false;
  };

  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    message.hidden = true;
    const next = form.next.value;
    if (next.length < 8) return fail('Das neue Passwort braucht mindestens 8 Zeichen.');
    if (next !== form.repeat.value) return fail('Die neuen Passwörter stimmen nicht überein.');

    button.disabled = true;
    try {
      await post('/api/auth/password', { current: form.current.value, new: next });
      form.reset();
      toast('Passwort geändert – bitte neu anmelden');
      // The server ended every session, this one included
      setTimeout(() => location.reload(), 1500);
    } catch (error) {
      fail(error.status === 403 ? 'Das aktuelle Passwort ist falsch.' : error.message);
      button.disabled = false;
    }
  });

  return section('Passwort ändern', 'Nach der Änderung werden alle Geräte abgemeldet.', form);
}

// Reads the newest config, applies change() to it and writes it back
async function updateConfig(change) {
  const config = await get('/api/config');
  change(config);
  await put('/api/config', config);
}

function displaySection(config) {
  const display = config.display || {};
  const form = settingsForm([
    field('Sprache und Region', select('locale', LOCALES, config.locale || 'de_DE'),
      'Für Uhrzeit, Datum und Zahlen auf der Anzeige'),
    field('Maximale Auflösung', select('maxRender', [['auto', 'Automatisch – 4K wenn möglich'], ['1080p', 'Höchstens Full HD']],
      display.maxRender === '1080p' ? '1080p' : 'auto'),
      'Beim Speichern startet die Anzeige kurz neu, um den Bildschirmmodus zu wechseln.'),
    field('Drehung', select('rotation', [[0, '0°'], [90, '90°'], [180, '180°'], [270, '270°']], display.rotation || 0),
      'Für hochkant oder kopfüber montierte Bildschirme'),
    field('Bildschirm abschalten über', select('power', POWER, ['cec', 'ddc'].includes(display.power) ? display.power : 'none'),
      'Wie der Zeitplan den Bildschirm ausschaltet. „Nur schwarzes Bild“ funktioniert immer.'),
    field('Profil', select('profile', PROFILES, config.profile === 'public' ? 'public' : 'home'),
      'Öffentlicher Raum: keine Eingabegeräte, kürzere Anmeldung. Wirkt nach einem Neustart der Dienste.'),
  ], (form) => updateConfig((current) => {
    current.locale = form.locale.value;
    current.profile = form.profile.value;
    current.display = {
      ...(current.display || {}),
      maxRender: form.maxRender.value,
      rotation: Number(form.rotation.value),
      power: form.power.value,
    };
  }));
  return section('Anzeige', null, form);
}

function librarySection(config) {
  const library = config.library || {};
  const admin = config.admin || {};
  const quality = el('input', { name: 'jpegQuality', type: 'number', min: 50, max: 100, step: 1, value: library.jpegQuality ?? 88, required: true });
  const upload = el('input', { name: 'maxUploadMB', type: 'number', min: 1, max: 128, step: 1, value: admin.maxUploadMB ?? 64, required: true });
  const form = settingsForm([
    el('div', { className: 'full' }, [field('Varianten', select('renditions', RENDITIONS, renditionsToChoice(library.renditions)),
      'Welche Bildgrößen im Hintergrund vorbereitet werden. Automatisch richtet sich nach der Auflösung der Anzeige.')]),
    field('JPEG-Qualität', quality, '50–100, Standard 88'),
    field('Maximale Upload-Größe (MB)', upload, '1–128, Standard 64'),
  ], (form) => updateConfig((current) => {
    current.library = {
      ...(current.library || {}),
      renditions: choiceToRenditions(form.renditions.value),
      jpegQuality: Math.min(100, Math.max(50, Math.round(Number(form.jpegQuality.value)))),
    };
    current.admin = {
      ...(current.admin || {}),
      maxUploadMB: Math.min(128, Math.max(1, Math.round(Number(form.maxUploadMB.value)))),
    };
  }));
  return section('Mediathek', null, form);
}

function adminSection(config) {
  const admin = config.admin || {};
  const readOnly = (label, value) => field(label, el('input', { value: String(value), readOnly: true }));
  return section('Verwaltung',
    'Adresse und Port lassen sich nur in config.json ändern (Abschnitt "admin"); danach den Dienst berry-kiosk-admin neu starten.',
    el('div', { className: 'form-grid two' }, [
      readOnly('Adresse', admin.address || '0.0.0.0'),
      readOnly('Port', admin.port || 8080),
      el('p', { className: 'muted small full set-hint', textContent:
        'Mit "enabled": false im Abschnitt "admin" lässt sich die Web-Verwaltung ganz abschalten – etwa für einen öffentlichen Kiosk, '
        + 'dessen Bilder per Dateifreigabe kommen. Die Mediathek wird trotzdem weiter gepflegt.' }),
    ]));
}

function aboutSection(version) {
  return section('Über', null, el('div', { className: 'stack set-about' }, [
    el('p', {}, [el('strong', { textContent: 'berry-kiosk ' }), el('span', { textContent: version || '' })]),
    el('p', { className: 'muted small', textContent: 'Bilderrahmen und Kiosk-Anzeige für den Raspberry Pi.' }),
    el('a', { href: 'https://github.com/herrlolstick/berry-kiosk', target: '_blank', rel: 'noopener', textContent: 'Projekt auf GitHub' }),
  ]));
}

export default {
  title: 'Einstellungen',

  async mount(root) {
    root.append(el('div', { className: 'page-header' }, [el('h1', { textContent: 'Einstellungen' })]));
    const content = el('div', { className: 'set-grid' }, [el('p', { className: 'muted', textContent: 'Lädt …' })]);
    root.append(content);

    let config;
    let version = '';
    try {
      const [loadedConfig, status] = await Promise.all([get('/api/config'), get('/api/status').catch(() => null)]);
      config = loadedConfig || {};
      version = status?.version || '';
    } catch (error) {
      content.replaceChildren(el('p', { className: 'error', textContent: `Einstellungen nicht ladbar: ${error.message}` }));
      return;
    }

    content.replaceChildren(
      passwordSection(),
      displaySection(config),
      librarySection(config),
      adminSection(config),
      aboutSection(version),
    );
  },
};
