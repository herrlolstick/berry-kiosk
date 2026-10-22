// Editor for the global quote list (config.quotes), shown by quote widgets
// when a picture has no quote of its own.

import { el } from './ui.js';

// onChange(quotes) after every edit
export function createQuotesEditor(initial, onChange) {
  let quotes = normalize(initial);
  const list = el('ol', { className: 'lay-quotes' });
  const element = el('section', { className: 'card stack' }, [
    el('div', { className: 'row' }, [
      el('h2', { className: 'spacer', textContent: 'Zitatliste' }),
      el('button', { type: 'button', className: 'small', textContent: '+ Zitat', onclick: add }),
    ]),
    el('p', { className: 'muted small', textContent: 'Zitat-Widgets zeigen sie der Reihe nach, wenn ein Bild kein eigenes Zitat hat.' }),
    list,
  ]);
  render();

  function normalize(value) {
    return (Array.isArray(value) ? value : [])
      .map((quote) => (typeof quote === 'string' ? { text: quote } : { text: quote?.text ?? '', author: quote?.author ?? '' }));
  }

  // Empty authors are left out, empty quotes are dropped on save
  const output = () => quotes
    .filter((quote) => quote.text.trim())
    .map((quote) => (quote.author?.trim() ? { text: quote.text.trim(), author: quote.author.trim() } : { text: quote.text.trim() }));

  function changed() {
    onChange(output());
  }

  function add() {
    quotes.push({ text: '', author: '' });
    render();
    list.lastElementChild?.querySelector('textarea')?.focus();
  }

  function move(index, delta) {
    const target = index + delta;
    if (target < 0 || target >= quotes.length) return;
    [quotes[index], quotes[target]] = [quotes[target], quotes[index]];
    render();
    changed();
  }

  function remove(index) {
    quotes.splice(index, 1);
    render();
    changed();
  }

  function render() {
    if (!quotes.length) {
      list.replaceChildren(el('li', { className: 'empty', textContent: 'Noch keine Zitate.' }));
      return;
    }
    list.replaceChildren(...quotes.map((quote, index) => {
      const text = el('textarea', { rows: 2, value: quote.text, placeholder: 'Zitat', attrs: { 'aria-label': `Zitat ${index + 1}` } });
      const author = el('input', { value: quote.author ?? '', placeholder: 'Von (optional)', attrs: { 'aria-label': `Urheber von Zitat ${index + 1}` } });
      text.addEventListener('input', () => { quote.text = text.value; changed(); });
      author.addEventListener('input', () => { quote.author = author.value; changed(); });
      return el('li', { className: 'lay-quote' }, [
        el('div', { className: 'stack' }, [text, author]),
        el('div', { className: 'lay-quote-actions' }, [
          el('button', { type: 'button', className: 'small secondary', textContent: '↑', title: 'Nach oben', disabled: index === 0, onclick: () => move(index, -1) }),
          el('button', { type: 'button', className: 'small secondary', textContent: '↓', title: 'Nach unten', disabled: index === quotes.length - 1, onclick: () => move(index, 1) }),
          el('button', { type: 'button', className: 'small secondary', textContent: 'Löschen', onclick: () => remove(index) }),
        ]),
      ]);
    }));
  }

  return {
    element,
    quotes: output,
    setQuotes(next) {
      quotes = normalize(next);
      render();
    },
  };
}
