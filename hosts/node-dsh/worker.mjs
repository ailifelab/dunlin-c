// Optional trusted-full-host process. The C caller owns permissions and effects.
import {registerHooks} from 'node:module';
import {pathToFileURL,fileURLToPath} from 'node:url';
import path from 'node:path';
import fs from 'node:fs/promises';
import readline from 'node:readline';
import {createRequire} from 'node:module';
import fsSync from 'node:fs';
import {Readable} from 'node:stream';
// Preserve original TypeScript locations in bounded local fault diagnostics.
process.setSourceMapsEnabled(true);
if(!/^[a-f0-9]{64}$/.test(process.env.DUNLIN_WORKER_NONCE ?? ''))throw Error('Native owner handshake required');
const toolchain=path.resolve(process.argv[3]);
const esbuild=createRequire(path.join(toolchain,'package.json'))('esbuild');
const compatibilityRoot=path.resolve(process.argv[2]);
registerHooks({resolve(specifier,context,next){
 const map={'@deepseek-ai/cordis':'cordis','@deepseek-ai/cosmokit':'cosmokit'};
 if(map[specifier])return {url:pathToFileURL(path.join(compatibilityRoot,'vendor',map[specifier],'src/index.ts')).href,shortCircuit:true};
 try{return next(specifier,context)}catch(error){
  if(error.code!=='ERR_MODULE_NOT_FOUND'||specifier.startsWith('.')||specifier.startsWith('/')||specifier.startsWith('node:'))throw error;
  return next(specifier,{...context,parentURL:pathToFileURL(path.join(toolchain,'package.json')).href});
 }
},load(url,context,next){
 if(url.startsWith('file:')&&/\.(ts|tsx)$/.test(fileURLToPath(url))){
  const source=fsSync.readFileSync(fileURLToPath(url),'utf8');
  return {format:'module',source:esbuild.transformSync(source,{loader:url.endsWith('.tsx')?'tsx':'ts',format:'esm',target:'es2022',sourcemap:'inline',sourcefile:fileURLToPath(url)}).code,shortCircuit:true};
 }
 return next(url,context);
}});
const {Context,FiberState,Service}=await import(pathToFileURL(path.join(compatibilityRoot,'vendor/cordis/src/index.ts')).href);
const pin=JSON.parse(await fs.readFile(path.join(compatibilityRoot,'pin.json'),'utf8'));
const root=new Context();
const plugins=new Map(),commands=new Map(),contributions=new Map(),owners=new WeakMap();
// Keep JSON-RPC stdout exclusively framed. Application diagnostics use stderr.
console.log=(...args)=>console.error(...args);
function owned(scope,key,value,registry){
 let fiber=scope.fiber,owner=owners.get(fiber);while(!owner&&fiber.parent?.fiber&&fiber.parent.fiber!==fiber){fiber=fiber.parent.fiber;owner=owners.get(fiber)}
 if(!owner)throw Error('unowned contribution');const full=owner+':'+key;if(registry.has(full))throw Error('duplicate contribution');
 registry.set(full,{owner,value});scope.fiber.effect(()=>()=>registry.delete(full));return full;
}
function descriptorValid(d){
 if(!d||typeof d!=='object'||Array.isArray(d)||!['command','settings','sidebar','theme'].includes(d.kind))return false;
 const string=(v,max)=>typeof v==='string'&&v.length>0&&Buffer.byteLength(v)<=max&&!v.includes('\0');
 for(const key of ['title','name','body','command','scheme'])if(d[key]!==undefined&&!string(d[key],key==='body'?16384:256))return false;
 for(const [key,fields] of [['actions',['id','title','command']],['sections',['id','title','body']]]){
  if(d[key]===undefined)continue;if(!Array.isArray(d[key])||d[key].length>32)return false;const ids=new Set();
  for(const row of d[key]){if(!row||typeof row!=='object'||fields.some(k=>!string(row[k],k==='body'?8192:256))||ids.has(row.id))return false;ids.add(row.id)}
 }
 return d.kind!=='command'||string(d.command,128);
}
root.provide('dunlin',Object.freeze({
 registerCommand(scope,id,handler){if(typeof id!=='string'||id.length>128||typeof handler!=='function')throw Error('invalid command');return owned(scope,id,handler,commands)},
 contribute(scope,id,descriptor){
  if(!descriptorValid(descriptor)||typeof id!=='string'||!id||id.length>128)throw Error('invalid native contribution');
  const bytes=JSON.stringify(descriptor);if(bytes.length>32768)throw Error('contribution too large');return owned(scope,id,JSON.parse(bytes),contributions)
 }
}));
// Explicit native projection of the pinned DSH ThemeDefinition/register contract.
// DOM/CSS layout, React slots and overrideTokens are separate capabilities.
const themeNames=new Set(['light','dark','system']);
const themeTokens={'--dsw-alias-brand-primary':'accent','--dsw-alias-bg-base':'background','--dsw-alias-bg-layer-1':'surface','--dsw-alias-label-primary':'text','--dsw-alias-label-secondary':'secondary'};
class NativeThemes extends Service {
 constructor(ctx){super(ctx,'theme')}
 register(definition){
  if(!definition||typeof definition.id!=='string'||!definition.id||definition.id.length>128||themeNames.has(definition.id)||!['light','dark'].includes(definition.colorScheme)||!definition.tokens||Array.isArray(definition.tokens))throw Error('unsupported theme definition');
  const tokens={accent:definition.colorScheme==='dark'?'#75ADFF':'#0066CC'};
  for(const [name,value] of Object.entries(definition.tokens)){
   if(!themeTokens[name]||typeof value!=='string'||!/^#[0-9a-fA-F]{6}$/.test(value))throw Error('unsupported native theme token');
   tokens[themeTokens[name]]=value;
  }
  const key=owned(this.ctx,'dsh-theme:'+definition.id,{kind:'theme',name:definition.id,title:definition.name ?? definition.id,scheme:definition.colorScheme,tokens},contributions);
  themeNames.add(definition.id);let disposed=false;
  const dispose=()=>{if(disposed)return;disposed=true;contributions.delete(key);themeNames.delete(definition.id)};
  this.ctx.effect(()=>dispose);return dispose;
 }
}
new NativeThemes(root);
const {installRoutes}=await import('./routes.mjs');
const routes=installRoutes(root,{owned,commands,contributions});

const {installQuestions}=await import('./questions.mjs');
const questions=installQuestions(root);
const {composeServices}=await import('./services.mjs');
const services=await composeServices(root,{owned,commands,contributions});

const methods={
 'system.info':()=>({kind:'node-dsh',node:process.versions.node,dsh:pin.dsh,commit:pin.commit,cordis:pin.cordis,services:['dunlin.contributions@1','dsh.theme.register@1','dsh.webServer.exact-json@1'],permissionModel:'trusted-full-host'}),
 'plugins.activate':async p=>{
  if(!p||typeof p.id!=='string'||typeof p.root!=='string'||typeof p.entry!=='string'||plugins.has(p.id))throw Error('invalid activation');
  const base=await fs.realpath(p.root),entry=await fs.realpath(path.resolve(base,p.entry));if(!entry.startsWith(base+path.sep))throw Error('entry escapes package');
  const mod=await import(pathToFileURL(entry).href);const plugin=mod.default ?? mod;
  const shape=typeof plugin==='function'?{...plugin,apply:plugin}:plugin;
  const required=Array.isArray(shape.inject)?shape.inject:Object.entries(shape.inject ?? {}).filter(([,required])=>required).map(([name])=>name);
  const missing=required.filter(name=>!root.get(name));if(missing.length)throw Object.assign(Error('Required services unavailable'),{code:'DUNLIN_MISSING_SERVICES',missingServices:missing});
  if(typeof shape.apply!=='function')throw Error('unsupported plugin entry');
  const wrapped={...shape,apply(ctx,config){
   owners.set(ctx.fiber,p.id);
   const projectThemes=()=>{
    const packageFile=path.join(base,'package.json');let pkg;try{pkg=JSON.parse(fsSync.readFileSync(packageFile,'utf8'))}catch{return}
    if(pkg.name!=='dsh-catppuccin')return;
    for(const flavor of ['latte','frappe','macchiato','mocha']){
     const file=path.join(base,'themes','catppuccin-'+flavor+'.json');if(fsSync.statSync(file).size>65536)throw Error('theme resource exceeds limit');
     const definition=JSON.parse(fsSync.readFileSync(file,'utf8'));const projection={...definition,tokens:{}};
     for(const key of Object.keys(themeTokens))if(definition.tokens[key]!==undefined)projection.tokens[key]=definition.tokens[key];
     ctx.theme.register(projection);
    }
   };
   const result=shape.apply(ctx,config);if(result&&typeof result.then==='function')return result.then(value=>{projectThemes();return value});projectThemes();return result;
  }};
  let fiber;try {fiber=await root.plugin(wrapped,p.config ?? {});if(!fiber||fiber.state!==FiberState.ACTIVE)throw Error('plugin did not activate');plugins.set(p.id,fiber);return {id:p.id,state:'active'}}catch(error){if(fiber)await fiber.dispose();throw error}
 },
 'plugins.deactivate':async p=>{questions.cancel(p.id);const fiber=plugins.get(p.id);if(!fiber)return {state:'inactive'};await fiber.dispose();plugins.delete(p.id);return {state:'inactive'}},
 'plugins.command':async p=>questions.run(p.plugin,p.nativeQuestions===true,()=>services.invoke(p.nativeContext,async()=>{if(p.command===p.plugin+':http.body')return routes.body(p.plugin,p.arguments ?? {});if(p.command===p.plugin+':http')return routes.request(p.plugin,p.arguments ?? {});const command=commands.get(p.command);if(!command||command.owner!==p.plugin)throw Object.assign(Error('unknown command'),{code:-32601});return await command.value(p.arguments ?? {})})),
 'plugins.answer':async p=>questions.answer(p.plugin,p),
 'plugins.contributions':()=>[...contributions].map(([id,{owner,value}])=>({...value,id,plugin:owner})),
 'plugins.tools':()=>services.tools(),
 'plugins.list':()=>[...plugins.keys()]
};
function diagnostic(error){
 const kind=typeof error?.name==='string' ? error.name.replace(/[^A-Za-z0-9_]/g,'').slice(0,128):'Error';
 const frames=String(error?.stack ?? '').split('\n').slice(1).filter(line=>/^\s*at /.test(line)).slice(0,8).map(line=>{
  const location=line.match(/([^/\\():\s]+):([0-9]+):([0-9]+)\)?$/);
  return location ? `${location[1].slice(0,128)}:${location[2]}:${location[3]}`:null;
 }).filter(Boolean);
 const moduleName=error?.code==='ERR_MODULE_NOT_FOUND'?String(error?.message ?? '').match(/Cannot find package '(@?[A-Za-z0-9_.\/-]+)'/)?.[1]:null;
 const missing=Array.isArray(error?.missingServices)?error.missingServices.filter(s=>typeof s==='string'&&/^[a-zA-Z0-9_.-]{1,128}$/.test(s)).slice(0,32):[];
 return [kind||'Error',...(moduleName?['Missing package: '+moduleName.slice(0,128)]:[]),...(missing.length?['Missing services: '+missing.join(', ')]:[]),...frames].join('\n').slice(0,2048);
}
const write=o=>process.stdout.write(JSON.stringify(o)+'\n');
write({protocol:'dunlin.worker',major:1,nonce:process.env.DUNLIN_WORKER_NONCE,methods:Object.keys(methods),runtime:{kind:'node-dsh',node:process.versions.node,dsh:pin.dsh,commit:pin.commit,cordis:pin.cordis}});
process.on('unhandledRejection',()=>process.exit(71));process.on('uncaughtException',()=>process.exit(72));
for await(const line of readline.createInterface({input:process.stdin,crlfDelay:Infinity})){
 if(Buffer.byteLength(line)>1048576)process.exit(73);
 let q;try{q=JSON.parse(line);if(q.jsonrpc!=='2.0'||!Number.isSafeInteger(q.id)||!Object.hasOwn(methods,q.method))throw Object.assign(Error('invalid method'),{code:-32601});const result=await methods[q.method](q.params);write({jsonrpc:'2.0',id:q.id,result:result ?? null})}
 catch(error){write({jsonrpc:'2.0',id:q?.id ?? null,error:{code:Number.isInteger(error.code)?error.code:-32603,message:error.code===-32601?'Method unavailable':'Plugin execution failed',diagnostic:diagnostic(error)}})}
}
await root.fiber.dispose();
