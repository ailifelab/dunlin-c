// Unchanged published client activation probe; no synthetic plugin substitute.
import fs from 'node:fs/promises';import path from 'node:path';import vm from 'node:vm';import {createRequire} from 'node:module';
const require=createRequire(path.resolve('build/v51/dsh-web-test/package.json'));const {JSDOM}=require('jsdom');
const inputs=JSON.parse(await fs.readFile('reports/v51-current/react-workbench-inputs.json','utf8'));let failed=0;
for(const input of inputs){
 const dom=new JSDOM('<!doctype html><html><head></head><body><div id="mount"></div></body></html>',{url:'https://owned.invalid',runScripts:'outside-only',pretendToBeVisual:true}),w=dom.window;
 w.console=console;w.TextEncoder=TextEncoder;w.TextDecoder=TextDecoder;w.ResizeObserver=class{observe(){}disconnect(){}};w.IntersectionObserver=class{observe(){}disconnect(){}};w.CSS={escape:s=>s};w.matchMedia=()=>({matches:false,addEventListener(){},removeEventListener(){}});w.fetch=async()=>{throw Error('No owner transport in activation-only probe')};
 const faults=[];let surface;
 try{
  vm.runInContext(await fs.readFile('build/v51/dsh-web-runtime/runtime.js','utf8'),dom.getInternalVMContext());let registration;w.__ModuleLoader__={load:r=>registration=r};vm.runInContext(await fs.readFile(input.client,'utf8'),dom.getInternalVMContext());
  surface=await w.DunlinDSH.createSurface('owned-workbench',f=>faults.push(f),{slot:'dunlin.workbench',theme:'light',request:async()=>{throw Error('No owner transport')}});await surface.load(registration,registration.id);surface.mount(w.document.getElementById('mount'));await new Promise(r=>setTimeout(r,100));if(faults.length)throw Error(JSON.stringify(faults));
  console.log(input.name,'ACTIVE',w.document.body.textContent.slice(0,300));
 }catch(error){failed++;console.log(input.name,'FAIL',String(error));}
 finally{await surface?.dispose();dom.window.close()}
}
process.exitCode=failed?1:0;
