// App shell: login, navigation and a small hash router. Views live in
// view-*.js and export default { title, mount(root, context) } where mount
// returns an optional cleanup function.
//
// context = { params, query, navigate(hash), session }
//   params  path segments after the route, e.g. #/mediathek/42 -> ['42']
//   query   URLSearchParams of the part after "?" in the hash

import { get, post, onUnauthorized } from './api.js';
import { el, toast } from './ui.js';

const routes = {
  '': { label: 'Übersicht', load: () => import('./view-dashboard.js') },
  mediathek: { label: 'Mediathek', load: () => import('./view-library.js') },
  hochladen: { label: 'Hochladen', load: () => import('./view-upload.js') },
  alben: { label: 'Alben', load: () => import('./view-albums.js') },
  layouts: { label: 'Layouts', load: () => import('./view-layouts.js') },
  zeitplan: { label: 'Zeitplan', load: () => import('./view-schedule.js') },
  einstellungen: { label: 'Einstellungen', load: () => import('./view-settings.js') },
};

const $ = (selector) => document.querySelector(selector);
let session = null;
let cleanup = null;
let renderId = 0;

export function navigate(hash) {
  if (location.hash === hash) render();
  else location.hash = hash;
}

function parseHash() {
  const [path, queryText = ''] = location.hash.replace(/^#\/?/, '').split('?');
  const segments = path.split('/').filter(Boolean).map(decodeURIComponent);
  const name = routes[segments[0]] ? segments[0] : '';
  return { name, params: name ? segments.slice(1) : [], query: new URLSearchParams(queryText) };
}

async function render() {
  if (!session) return;
  const id = ++renderId;
  const { name, params, query } = parseHash();
  const route = routes[name];

  for (const link of document.querySelectorAll('#nav a')) {
    link.classList.toggle('active', link.dataset.route === name);
  }

  if (cleanup) {
    try { cleanup(); } catch (error) { console.error(error); }
    cleanup = null;
  }
  const root = $('#view');
  root.replaceChildren(el('p', { className: 'muted', textContent: 'Lädt …' }));

  try {
    const module = await route.load();
    if (id !== renderId) return; // navigated away meanwhile
    const view = module.default;
    document.title = `${view.title || route.label} · berry-kiosk`;
    root.replaceChildren();
    cleanup = (await view.mount(root, { params, query, navigate, session })) || null;
  } catch (error) {
    console.error(error);
    if (id === renderId) root.replaceChildren(el('p', { className: 'error', textContent: `Ansicht nicht ladbar: ${error.message}` }));
  }
}

function buildNav() {
  const nav = $('#nav');
  nav.replaceChildren(...Object.entries(routes).map(([name, route]) =>
    el('a', { href: `#/${name}`, textContent: route.label, dataset: { route: name } })));
}

function showLogin(message = '') {
  session = null;
  if (cleanup) cleanup();
  cleanup = null;
  $('#app').hidden = true;
  $('#login').hidden = false;
  const error = $('#login-error');
  error.textContent = message;
  error.hidden = !message;
  $('#login-form').password.value = '';
  $('#login-form').password.focus();
}

function showApp() {
  $('#login').hidden = true;
  $('#app').hidden = false;
  $('#user').textContent = session.username;
  render();
}

async function login(event) {
  event.preventDefault();
  const form = event.target;
  const button = form.querySelector('button');
  button.disabled = true;
  try {
    await post('/api/auth/login', { username: form.username.value.trim(), password: form.password.value });
    session = await get('/api/auth/session');
    showApp();
  } catch (error) {
    showLogin(error.message);
  } finally {
    button.disabled = false;
  }
}

async function logout() {
  try {
    await post('/api/auth/logout');
  } catch (error) {
    toast(error.message, 'error');
  }
  showLogin();
}

async function start() {
  buildNav();
  $('#login-form').addEventListener('submit', login);
  $('#logout').addEventListener('click', logout);
  $('#menu').addEventListener('click', () => document.body.classList.toggle('nav-open'));
  $('#nav').addEventListener('click', () => document.body.classList.remove('nav-open'));
  window.addEventListener('hashchange', render);
  onUnauthorized(() => showLogin('Bitte erneut anmelden.'));

  try {
    const current = await get('/api/auth/session');
    if (current.authenticated) {
      session = current;
      showApp();
    } else {
      showLogin(current.passwordSet ? '' : 'Noch kein Passwort gesetzt. Auf dem Pi: berry-kiosk-admin --set-password');
    }
  } catch (error) {
    showLogin(`Server nicht erreichbar: ${error.message}`);
  }
}

start();
