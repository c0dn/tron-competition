import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const { JSDOM } = require('jsdom') as {
  JSDOM: new (html: string, options: { url: string }) => { window: Window & typeof globalThis };
};

if (typeof document === 'undefined') {
  const dom = new JSDOM('<!doctype html><html><body></body></html>', { url: 'http://localhost' });
  Object.assign(globalThis, {
    window: dom.window,
    document: dom.window.document,
    navigator: dom.window.navigator,
    HTMLElement: dom.window.HTMLElement,
    Node: dom.window.Node,
    Event: dom.window.Event,
    KeyboardEvent: dom.window.KeyboardEvent,
    MouseEvent: dom.window.MouseEvent,
    getComputedStyle: dom.window.getComputedStyle,
  });
}

const nativeFetch = globalThis.fetch;

export function stubFetch(fetchMock: unknown): void {
  Object.defineProperty(globalThis, 'fetch', { configurable: true, writable: true, value: fetchMock });
}

export function restoreFetch(): void {
  Object.defineProperty(globalThis, 'fetch', { configurable: true, writable: true, value: nativeFetch });
}
