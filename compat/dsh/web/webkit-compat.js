// Legacy WebKitGTK loads this before the shared client and third-party modules.
(() => {
  const define = (target, name, value) => {
    if (!(name in target)) Object.defineProperty(target, name, {value, writable: true, configurable: true});
  };
  define(Object, 'hasOwn', (value, key) => Object.prototype.hasOwnProperty.call(value, key));
  for (const prototype of [Array.prototype, String.prototype, Object.getPrototypeOf(Uint8Array.prototype)]) {
    define(prototype, 'at', function (index) {
      const n = Math.trunc(Number(index)) || 0;
      const offset = n < 0 ? this.length + n : n;
      return offset < 0 || offset >= this.length ? undefined : this[offset];
    });
  }
  if (globalThis.crypto?.getRandomValues) define(globalThis.crypto, 'randomUUID', function () {
    const bytes = globalThis.crypto.getRandomValues(new Uint8Array(16));
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    const hex = Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('');
    return [hex.slice(0, 8), hex.slice(8, 12), hex.slice(12, 16), hex.slice(16, 20), hex.slice(20)].join('-');
  });
  // Some desktop components query the focus ring from their focus handler. Keep
  // the native selector engine for all matching and syntax validation.
  function installFocusVisible() {
    if (!globalThis.document || !globalThis.Element) return;
    const element = Element.prototype;
    try { document.documentElement.matches(':focus-visible'); return; }
    catch (error) { if (error.name !== 'SyntaxError') throw error; }

    const marker = 'data-dunlin-focus-visible';
    const editingInputs = new Set(['text', 'search', 'url', 'tel', 'email', 'password',
      'number', 'date', 'month', 'week', 'time', 'datetime-local']);
    let usingKeyboard = true;
    let marked = null;
    function showRing(target) {
      if (marked && marked !== target) marked.removeAttribute(marker);
      marked = target;
      if (target) target.setAttribute(marker, '');
    }
    function update(target) {
      if (!target || target.nodeType !== 1) { showRing(null); return; }
      const editing = target.isContentEditable || (!target.readOnly &&
        (target.localName === 'textarea' || (target.localName === 'input' && editingInputs.has(target.type))));
      showRing(usingKeyboard || editing ? target : null);
    }
    function focused() {
      let node = document.activeElement;
      while (node && node.shadowRoot && node.shadowRoot.activeElement) node = node.shadowRoot.activeElement;
      return node;
    }
    document.addEventListener('keydown', event => {
      if (event.metaKey || event.altKey || event.ctrlKey) return;
      usingKeyboard = true;
      update(focused());
    }, true);
    const pointer = () => { usingKeyboard = false; update(focused()); };
    for (const name of ['pointerdown', 'mousedown', 'touchstart']) document.addEventListener(name, pointer, true);
    document.addEventListener('focus', event => {
      update(event.composedPath ? event.composedPath()[0] : event.target);
    }, true);
    document.addEventListener('blur', () => showRing(null), true);
    update(focused());

    // CSS escapes, strings, comments and attribute values may contain the text
    // ":focus-visible" without referring to the pseudo-class.
    const tokens = /\\[\s\S]|\/\*[\s\S]*?(?:\*\/|$)|"(?:\\[\s\S]|[^"\\])*(?:"|$)|'(?:\\[\s\S]|[^'\\])*(?:'|$)|\[|\]|::?focus-visible(?![-\w\u0080-\uFFFF\\])/gi;
    function translate(selector) {
      if (typeof selector !== 'string' || !/:focus-visible/i.test(selector)) return selector;
      let inAttribute = 0;
      return selector.replace(tokens, token => {
        if (token === '[') inAttribute++;
        if (token === ']') inAttribute--;
        return !inAttribute && token.toLowerCase() === ':focus-visible' ? ':focus[' + marker + ']' : token;
      });
    }
    function wrap(target, name) {
      const descriptor = Object.getOwnPropertyDescriptor(target, name);
      if (!descriptor || typeof descriptor.value !== 'function') return;
      const original = descriptor.value;
      descriptor.value = function (selector) {
        return arguments.length ? original.call(this, translate(selector)) : original.call(this);
      };
      Object.defineProperty(target, name, descriptor);
    }
    for (const name of ['matches', 'webkitMatchesSelector', 'closest']) wrap(element, name);
    for (const prototype of [element, Document.prototype, DocumentFragment.prototype])
      for (const name of ['querySelector', 'querySelectorAll']) wrap(prototype, name);
  }
  installFocusVisible();
  // esbuild lowers regex literals to constructor calls on older JavaScriptCore.
  // Preserve captures, match.index and lastIndex for fixed-prefix lookbehind.
  const Original = RegExp;
  let lookbehind = true;
  try { new Original('(?<!x)y'); } catch (_) { lookbehind = false; }
  if (!lookbehind) {
    class PrefixRegExp extends Original {
      constructor(pattern, flags) {
        const source = pattern instanceof Original ? pattern.source : String(pattern);
        const prefix = /^\(\?<([!=])((?:\\[!.,:\-]|[A-Za-z0-9 _!,.:\-])+)\)/.exec(source);
        if (!prefix) { super(pattern, flags); return; }
        const test = new Original('(?:' + prefix[2] + ')(?![\\s\\S])', flags && flags.includes('i') ? 'i' : '');
        super(source.slice(prefix[0].length), flags === undefined && pattern instanceof Original ? pattern.flags : flags);
        this.prefixTest = test;
        this.prefixPositive = prefix[1] === '=';
        this.originalSource = source;
      }
      get source() { return this.originalSource || super.source; }
      exec(input) {
        if (!this.prefixTest) return super.exec(input);
        const text = String(input);
        const search = new Original(super.source, this.flags.includes('g') ? this.flags : this.flags + 'g');
        search.lastIndex = this.global || this.sticky ? this.lastIndex : 0;
        let match;
        while ((match = search.exec(text))) {
          if (this.prefixTest.test(text.slice(0, match.index)) === this.prefixPositive) {
            if (this.global || this.sticky) this.lastIndex = search.lastIndex;
            return match;
          }
          if (this.sticky) break;
          const cp = text.codePointAt(match.index);
          search.lastIndex = match.index + (this.unicode && cp > 65535 ? 2 : 1);
        }
        if (this.global || this.sticky) this.lastIndex = 0;
        return null;
      }
    }
    const wrapped = new Proxy(Original, {
      construct(target, args) { return new PrefixRegExp(...args); },
      apply(target, receiver, args) { return new PrefixRegExp(...args); }
    });
    globalThis.RegExp = wrapped;
  }
  define(Object, 'groupBy', (iterable, callback) => {
    const groups = Object.create(null);
    let index = 0;
    for (const value of iterable) {
      const key = callback(value, index++);
      (groups[key] || (groups[key] = [])).push(value);
    }
    return groups;
  });
})();
