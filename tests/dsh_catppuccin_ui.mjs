// Unchanged upstream client inside actual DOM/React/slot/theme services.
// Persistence transport is an owned fixture here; native/C checks are separate.
import fs from 'node:fs/promises';import path from 'node:path';import vm from 'node:vm';import assert from 'node:assert/strict';import {createRequire} from 'node:module';
const require=createRequire(path.resolve('build/v51/dsh-web-test/package.json'));const {JSDOM}=require('jsdom');
const dom=new JSDOM('<!doctype html><html><head></head><body><div id="surface"></div></body></html>',{url:'https://owned.invalid',runScripts:'outside-only',pretendToBeVisual:true});const w=dom.window;w.console=console;w.TextEncoder=TextEncoder;w.TextDecoder=TextDecoder;
let saved={},writes=0;w.fetch=async(url,init={})=>{if(url==='/catppuccin/state'){if(init.method==='PUT'){saved=JSON.parse(init.body);writes++}return {ok:true,json:async()=>saved}}return {ok:true,json:async()=>({ok:true,current:'0.2.6',latest:'0.2.6'})}};
const clientPath=process.argv[2]??'build/v51/real-dsh-published/dsh-catppuccin-0.2.6/lib/client.js';
const context=dom.getInternalVMContext();vm.runInContext(await fs.readFile('build/v51/dsh-web-runtime/runtime.js','utf8'),context);let registration;w.__ModuleLoader__={load:r=>registration=r};vm.runInContext(await fs.readFile(clientPath,'utf8'),context);
const faults=[],themes=[],snapshots=[];
const rpc=async(method,args)=>{
 if(method==='storage.read')return {'owned-state':'retained'};
 assert.equal(method,'storage.write');
 snapshots.push({...args.value});
};
await w.DunlinDSH.mountNativeSurface(w.DunlinDSH,registration,{chat:'owned-theme',package:registration.id,
 config:{},slot:'settings.general.item',theme:'dark'},rpc,f=>faults.push(f),t=>themes.push(t));
const surface=w.surface;
assert.equal(w.localStorage.getItem('owned-state'),'retained');
w.localStorage.setItem('ordered','one');w.localStorage.setItem('ordered','two');
await new Promise(resolve=>setTimeout(resolve,10));
assert.deepEqual(snapshots.filter(v=>v.ordered).slice(-2).map(v=>v.ordered),['one','two']);
assert.equal(w.DunlinDSH.surfaceDiagnostic({error:new ReferenceError('private'),lineno:42}),'ReferenceError line 42');
const settle=()=>new Promise(r=>setTimeout(r,50));await settle();
const button=label=>[...w.document.querySelectorAll('button')].find(b=>b.textContent.trim()===label);
for(const [label,id]of [['Latte','latte'],['Frappé','frappe'],['Macchiato','macchiato'],['Mocha','mocha']]){
 const b=button(label);assert.ok(b,'missing original flavor '+label);b.click();await settle();assert.equal(b.getAttribute('aria-pressed'),'true');assert.equal(themes.at(-1),'catppuccin-'+id);assert.equal(w.document.body.hasAttribute('data-ds-dark-theme'),id!=='latte','dark-theme presence marker mismatches actual scheme');
}
if(process.argv.includes('--flavors-only')){assert.equal(faults.length,0);await surface.dispose();dom.window.close();console.log('PASS unchanged older Catppuccin client, four flavors and cleanup');process.exit(0)}
const glow=button('粉彩光效')??button('Pastel glow');assert.ok(glow);const old=glow.getAttribute('aria-pressed');glow.click();await settle();assert.notEqual(glow.getAttribute('aria-pressed'),old);
const glass=w.document.querySelector('[role=switch]');assert.ok(glass);glass.click();await settle();assert.equal(glass.getAttribute('aria-checked'),'true');assert.ok(w.document.querySelectorAll('input').length>=3,'glass knobs absent');
await new Promise(r=>setTimeout(r,400));assert.ok(writes>0);assert.equal(saved.skin,'catppuccin-mocha');assert.equal(saved.glass.enabled,true);
surface.setTheme('light');await settle();assert.equal(w.document.body.hasAttribute('data-ds-dark-theme'),false);assert.equal(themes.at(-1),'light');await new Promise(r=>setTimeout(r,400));assert.equal(saved.skin,null,'native builtin did not replace stored skin');
assert.equal(faults.length,0);await surface.dispose();assert.equal(w.document.getElementById('surface').textContent,'');dom.window.close();
console.log('PASS real Catppuccin four flavor controls, active state/colors, glow, full glass controls, debounced state write, native builtin override and React cleanup; native appearance checked separately.');
