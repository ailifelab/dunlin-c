// Exercise the compatibility shim with a VM whose regexp constructor has the
// same missing lookbehind feature as WebKitGTK 2.32. Native browser coverage
// uses the installed GTK app separately.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { webcrypto } from 'node:crypto';

const shim = fs.readFileSync(new URL('../../../compat/dsh/web/webkit-compat.js', import.meta.url), 'utf8');
const runtime = vm.createContext({crypto: {getRandomValues: a => webcrypto.getRandomValues(a)}});
vm.runInContext(`{
  const original = RegExp;
  function legacy(pattern, flags) {
    if (typeof pattern === 'string' && /\\(\\?<([=!])/.test(pattern)) throw SyntaxError('lookbehind');
    return Reflect.construct(original, [pattern, flags], new.target || original);
  }
  legacy.prototype = original.prototype;
  globalThis.RegExp = legacy;
  Object.hasOwn = undefined;
  delete Object.hasOwn;
}`, runtime);
vm.runInContext(shim, runtime);
const evaluate = source => vm.runInContext(source, runtime);
assert.equal(evaluate('Object.hasOwn({a:0}, "a")'), true);
assert.match(evaluate('crypto.randomUUID()'), /^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$/);
assert.equal(evaluate('"x !x xx".replace(new RegExp("(?<!!)x","g"),"a")'), 'a !x aa');
assert.equal(evaluate('"aaa".replace(new RegExp("(?<=a)(a)","g"),"$1!")'), 'aa!a!');
assert.equal(evaluate('(()=>{let p=new RegExp("(?<!!)x","y");p.lastIndex=1;return p.exec("!x")===null&&p.lastIndex===0})()'), true);
assert.equal(evaluate('(()=>{let p=new RegExp("(?<=ab)x","gi");let m=p.exec("ABx");return [m.index,p.lastIndex,m[0]].join(",")})()'), '2,3,x');
assert.equal(evaluate('"🐦a!".replace(new RegExp("(?<!!)(?=.)","gu"),"|")'), '|🐦|a|!');
if (!process.argv.includes('--focus-script')) console.log('PASS WebKitGTK 2.32 host polyfills, Unicode-safe scanning, capture and cursor semantics');

// Print a self-contained probe for a native WebKitWebView. It uses the real
// browser selector engine, DOM focus, event dispatch and native method identity.
function focusRegression() {
  const check = (value, label) => { if (!value) throw Error(label); };
  const invalid = fn => {
    try { fn(); } catch (error) { return error.name === 'SyntaxError'; }
    return false;
  };
  const root = document.createElement('section');
  root.innerHTML = '<button class="card" data-name=":focus-visible"><span>Task</span></button>' +
    '<input type="text"><textarea></textarea><input type="checkbox"><input readonly>' +
    '<div contenteditable="true"></div><i class=":focus-visible"></i>';
  document.body.appendChild(root);
  const [card, text, area, checkbox, readonly, editor, literal] = root.children;
  const child = card.firstChild;
  const selectors = [Element.prototype.matches, Element.prototype.closest, Document.prototype.querySelector,
    DocumentFragment.prototype.querySelectorAll];
  check(originalFocusMethods.every((fn, i) => (fn === selectors[i]) === nativeFocus), 'capability gate');
  check(invalid(() => document.querySelector('button:not(')), 'native validation');
  check(invalid(() => card.matches(':focus-visible, :no-such-focus-state')), 'unknown selector in list');
  check(invalid(() => card.matches(':focus-visible(')), 'invalid focus function');
  check(invalid(() => card.matches(':focus-visible-extra')), 'longer pseudo-class name');
  check(root.querySelector('[data-name=":focus-visible"]') === card, 'quoted attribute');
  check(root.querySelector('.\\:focus-visible') === literal, 'escaped class');
  check(root.querySelector('button/* :focus-visible */') === card, 'CSS comment');
  check(root.querySelector('button') === card && child.closest('section') === root, 'ordinary selectors');
  const fragment = document.createDocumentFragment();
  fragment.appendChild(document.createElement('button'));
  check(fragment.querySelectorAll('button:not(:focus-visible)').length === 1, 'fragment selector');
  const key = options => document.dispatchEvent(new KeyboardEvent('keydown', Object.assign({key:'Tab', bubbles:true}, options)));
  const pointer = target => target.dispatchEvent(new MouseEvent('mousedown', {bubbles:true}));
  pointer(card);
  card.focus();
  if (!nativeFocus) {
    check(!child.closest('.card:focus-visible'), 'pointer-focused card has no keyboard ring');
    key({ctrlKey:true});
    check(!card.matches(':focus-visible'), 'modifier chord preserves modality');
    key();
    check(card.matches(':FOCUS-VISIBLE') && card.webkitMatchesSelector(':focus-visible'), 'keyboard and vendor alias');
    check(child.closest('button:focus-visible') === card, 'closest');
    check(document.querySelector('aside, section > button.card:focus-visible span') === child, 'selector list');
    check(root.querySelector('button:focus-visible > span') === child, 'ancestor selector');
    check(root.querySelectorAll('.card:not(:focus-visible), input:focus-visible').length === 0, 'negation');
    pointer(card);
    check(!card.matches(':focus-visible'), 'switch to pointer on focused card');
    for (const input of [text, area, editor]) {
      pointer(input); input.focus();
      check(input.matches(':focus-visible'), 'text editing retains visible focus: ' + input.localName);
      check(root.querySelectorAll(':focus-visible').length === 1, 'only focused target marked');
    }
    for (const input of [checkbox, readonly]) {
      pointer(input); input.focus();
      check(!input.matches(':focus-visible'), 'pointer on non-editing control');
    }
    key(); card.focus(); card.blur();
    check(!card.matches(':focus-visible'), 'blur clears marker');
  }
  root.remove();
  return 'PASS ' + (nativeFocus ? 'native :focus-visible unchanged' :
    'legacy :focus-visible pointer/keyboard/editing, selector lists, ancestry, escaping, validation and blur');
}
if (process.argv.includes('--focus-script')) {
  const start = `const originalFocusMethods = [Element.prototype.matches, Element.prototype.closest,
    Document.prototype.querySelector, DocumentFragment.prototype.querySelectorAll];
    let nativeFocus = true;
    try { document.documentElement.matches(':focus-visible'); } catch (error) {
      if (error.name !== 'SyntaxError') throw error;
      nativeFocus = false;
    }`;
  process.stdout.write(`(() => {\n${start}\n${shim}\nreturn (${focusRegression.toString()})();\n})()\n`);
}
