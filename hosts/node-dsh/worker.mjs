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
const toolRequire=createRequire(path.join(toolchain,'package.json'));
const esbuild=toolRequire('esbuild'),semver=toolRequire('semver');
const compatibilityRoot=path.resolve(process.argv[2]);
const serverSources=new Map();
const {checkpointLiveToolRepair,imageNativeWorkspaceRepair,projectTitleEffortRepair,projectUnknownHistoryTimeRepair,projectUnknownActivityTimeRepair,projectCalendarRevisionRepair,nativeSourceTimeRepair}=await import('./server_patches.mjs');
registerHooks({resolve(specifier,context,next){
 const map={'@deepseek-ai/cordis':'cordis','@deepseek-ai/cosmokit':'cosmokit'};
 if(map[specifier])return {url:pathToFileURL(path.join(compatibilityRoot,'vendor',map[specifier],'src/index.ts')).href,shortCircuit:true};
 try{return next(specifier,context)}catch(error){
  if(error.code!=='ERR_MODULE_NOT_FOUND'||specifier.startsWith('.')||specifier.startsWith('/')||specifier.startsWith('node:'))throw error;
  return next(specifier,{...context,parentURL:pathToFileURL(path.join(toolchain,'package.json')).href});
 }
},load(url,context,next){
 if(serverSources.has(url))return {format:'module',source:serverSources.get(url),shortCircuit:true};
 if(url===pathToFileURL(path.join(toolchain,'node_modules/@deepseek-ai/dsh-session/lib/index.js')).href){
  const file=fileURLToPath(url);if(fsSync.statSync(file).size>16777216)throw Error('Pinned session SDK exceeds source limit');
  const repair=nativeSourceTimeRepair(fsSync.readFileSync(file,'utf8'));if(!repair)throw Error('Pinned native session source-time SDK mismatch');
  return {format:'module',source:repair.source,shortCircuit:true};
 }
 if(url.startsWith('file:')&&/\.(ts|tsx)$/.test(fileURLToPath(url))){
  const source=fsSync.readFileSync(fileURLToPath(url),'utf8');
  return {format:'module',source:esbuild.transformSync(source,{loader:url.endsWith('.tsx')?'tsx':'ts',format:'esm',target:'es2022',sourcemap:'inline',sourcefile:fileURLToPath(url)}).code,shortCircuit:true};
 }
 return next(url,context);
}});
const {Context,FiberState,Service}=await import(pathToFileURL(path.join(compatibilityRoot,'vendor/cordis/src/index.ts')).href);
const pin=JSON.parse(await fs.readFile(path.join(compatibilityRoot,'pin.json'),'utf8'));
const root=new Context();
const metadata=new Map();
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
  const tokens={accent:definition.colorScheme==='dark'?'#75ADFF':'#0066CC'},webTokens={};
  if(Object.keys(definition.tokens).length>256)throw Error('Too many theme tokens');
  for(const [name,value] of Object.entries(definition.tokens)){
   if(!/^--(?:dsw|shiki)-[a-zA-Z0-9-]{1,110}$/.test(name)||typeof value!=='string'||value.length>256||!(/^(?:#[0-9a-fA-F]{6}|rgba\([0-9., ]+\)|color-mix\(in srgb, #[0-9a-fA-F]{6} [0-9]{1,3}%, #[0-9a-fA-F]{6}\))$/.test(value)))throw Error('unsupported web theme token');
   webTokens[name]=value;if(themeTokens[name]&&/^#[0-9a-fA-F]{6}$/.test(value))tokens[themeTokens[name]]=value;
  }
  const key=owned(this.ctx,'dsh-theme:'+definition.id,{kind:'theme',name:definition.id,title:definition.name ?? definition.id,scheme:definition.colorScheme,tokens,webTokens},contributions);
  themeNames.add(definition.id);let disposed=false;
  const dispose=()=>{if(disposed)return;disposed=true;contributions.delete(key);themeNames.delete(definition.id)};
  this.ctx.effect(()=>dispose);return dispose;
 }
}
new NativeThemes(root);
const {installRoutes}=await import('./routes.mjs');
const routes=installRoutes(root,{owned,commands,contributions,observe:id=>services.observe(id)});

const {installQuestions}=await import('./questions.mjs');
const questions=installQuestions(root);
const {nativeBridge}=await import('./native_bridge.mjs');
const native=nativeBridge();
const {composeServices}=await import('./services.mjs');
const services=await composeServices(root,{owned,commands,contributions,native});
const {installWorkbenchServices}=await import("./workbench.mjs");
const workbench=installWorkbenchServices(root,{owned,commands,contributions,metadata,native});

const methods={
 'system.info':()=>({kind:'node-dsh',node:process.versions.node,dsh:pin.dsh,commit:pin.commit,cordis:pin.cordis,services:['dunlin.contributions@1','dsh.theme.register@1','dsh.webServer.exact-json@1'],permissionModel:'trusted-full-host'}),
 'plugins.activate':async p=>{
  if(!p||typeof p.id!=='string'||typeof p.root!=='string'||typeof p.entry!=='string'||plugins.has(p.id))throw Error('invalid activation');
  const base=await fs.realpath(p.root),entry=await fs.realpath(path.resolve(base,p.entry));if(!entry.startsWith(base+path.sep))throw Error('entry escapes package');
  const entryUrl=pathToFileURL(entry).href;
  // Check declared engine ranges before evaluating any third-party module.
  let descriptor;
  try {descriptor=JSON.parse(await fs.readFile(path.join(base,'package.json'),'utf8'))} catch(error){if(error.code!=='ENOENT')throw error}
  for(const [engine,version,range] of [['node',process.versions.node,descriptor?.engines?.node],['dsh',pin.dsh,descriptor?.dsh?.engines?.dsh ?? descriptor?.engines?.dsh]]){
   if(range!==undefined&&(typeof range!=='string'||range.length>256||!semver.satisfies(version,range,{includePrerelease:true})))
    throw Object.assign(Error('Incompatible plugin engine'),{incompatibleEngine:engine})
  }
  if(p.entry==='index.mjs'||p.entry==='lib/index.js'){
   const pkg=JSON.parse(await fs.readFile(path.join(base,'package.json'),'utf8'));
   if((pkg.name==='dsh-checkpoint-rewind'&&pkg.version==='0.6.34')||(pkg.name==='dsh-image-gen'&&pkg.version==='0.11.0')||(pkg.name==='dsh-project-manager'&&pkg.version==='0.57.0')){
    if(fsSync.statSync(entry).size>16777216)throw Error('Plugin entry exceeds repair source limit');
    const original=await fs.readFile(entry,'utf8'),repair=checkpointLiveToolRepair(pkg,original)??imageNativeWorkspaceRepair(pkg,original)??projectTitleEffortRepair(pkg,original);if(repair)serverSources.set(entryUrl,repair.source);
    if(pkg.name==='dsh-project-manager'){
     const history=await fs.realpath(path.join(path.dirname(entry),'history.js'));if(!history.startsWith(base+path.sep)||fsSync.statSync(history).size>65536)throw Error('Invalid pinned Project history source');
     const timeRepair=projectUnknownHistoryTimeRepair(pkg,await fs.readFile(history,'utf8'));if(timeRepair)serverSources.set(pathToFileURL(history).href,timeRepair.source);
     const gantt=await fs.realpath(path.join(path.dirname(entry),'gantt.js'));if(!gantt.startsWith(base+path.sep)||fsSync.statSync(gantt).size>262144)throw Error('Invalid calendar source');
     const revisionRepair=projectCalendarRevisionRepair(pkg,await fs.readFile(gantt,'utf8'));if(revisionRepair)serverSources.set(pathToFileURL(gantt).href,revisionRepair.source);
     const days=await fs.realpath(path.join(path.dirname(entry),'sessiondays.js'));if(!days.startsWith(base+path.sep)||fsSync.statSync(days).size>65536)throw Error('Invalid pinned Project activity source');
     const daysRepair=projectUnknownActivityTimeRepair(pkg,await fs.readFile(days,'utf8'));if(daysRepair)serverSources.set(pathToFileURL(days).href,daysRepair.source);
    }
   }
  }
  const mod=await import(entryUrl);const plugin=mod.default ?? mod;
  const shape=typeof plugin==='function'?{...plugin,apply:plugin}:plugin;
  const required=Array.isArray(shape.inject)?shape.inject:Object.keys(shape.inject ?? {});
  const missing=required.filter(name=>!root.get(name));if(missing.length)throw Object.assign(Error('Required services unavailable'),{code:'DUNLIN_MISSING_SERVICES',missingServices:missing});
  if(typeof shape.apply!=='function')throw Error('unsupported plugin entry');
  const wrapped={...shape,apply(ctx,config){
   owners.set(ctx.fiber,p.id);
   const projectThemes=()=>{
    const packageFile=path.join(base,'package.json');let pkg;try{pkg=JSON.parse(fsSync.readFileSync(packageFile,'utf8'))}catch{return}
    if(pkg.name!=='dsh-catppuccin')return;
    for(const flavor of ['latte','frappe','macchiato','mocha']){
     const file=path.join(base,'themes','catppuccin-'+flavor+'.json');if(fsSync.statSync(file).size>65536)throw Error('theme resource exceeds limit');
     const definition=JSON.parse(fsSync.readFileSync(file,'utf8'));ctx.theme.register(definition);
    }
   };
   const result=shape.apply(ctx,config);if(result&&typeof result.then==='function')return result.then(value=>{projectThemes();return value});projectThemes();return result;
  }};
  let fiber;try {fiber=await root.plugin(wrapped,p.config ?? {});if(!fiber||fiber.state!==FiberState.ACTIVE)throw Error('plugin did not activate');let packageName='';try{packageName=JSON.parse(await fs.readFile(path.join(base,'package.json'),'utf8')).name}catch{}metadata.set(p.id,{name:packageName});plugins.set(p.id,fiber);return {id:p.id,state:'active'}}catch(error){if(fiber)await fiber.dispose();throw error}
 },
 'plugins.deactivate':async p=>{questions.cancel(p.id);await routes.close(p.id,undefined,true);metadata.delete(p.id);const fiber=plugins.get(p.id);if(!fiber)return {state:'inactive'};await fiber.dispose();plugins.delete(p.id);return {state:'inactive'}},
 'plugins.command':async p=>questions.run(p.plugin,p.nativeQuestions===true,()=>services.invoke(p.nativeContext,async()=>{if(p.command===p.plugin+':ui.settings.validate')return workbench.settings(p.plugin,p.arguments?.config);if(p.command===p.plugin+':ui.settings.apply')return workbench.settings(p.plugin,p.arguments?.config,true);if(p.command===p.plugin+':ui.prompt')return services.prompt(p.arguments);if(p.command===p.plugin+':ui.context')return services.context();if(p.command===p.plugin+':ui.workspaces')return workbench.workspaces();if(p.command===p.plugin+':ui.workspace')return workbench.workspace(p.arguments?.id);if(p.command===p.plugin+':ui.adoptSession')return {adopted:true};if(p.command===p.plugin+':http.pending')return routes.pending(p.plugin,p.arguments??{});if(p.command===p.plugin+':http.events')return routes.events(p.plugin,p.arguments??{});if(p.command.startsWith(p.plugin+':http.upload.'))return routes.upload(p.plugin,p.command.slice((p.plugin+':http.upload.').length),p.arguments??{});if(p.command===p.plugin+':ui.close'){await routes.close(p.plugin,p.arguments?._surface);for(const [key,row]of commands)if(row.owner===p.plugin&&key.startsWith(p.plugin+':ws:'))await row.value({op:'closeSurface',_surface:p.arguments?._surface});return {closed:true}}if(p.command===p.plugin+':rpc'){
   const args=p.arguments??{}
   if(args.namespace==='cotTranslation'&&['limits','translate'].includes(args.method)){
    const handler=commands.get(p.plugin+':'+args.namespace+'.'+args.method)
    if(!handler||handler.owner!==p.plugin)return {ok:false,error:{code:'service_unavailable',message:'Translation service unavailable'}}
    return await handler.value(args.args?.[0]??{})
   }
   return workbench.remote(p.plugin,args)
  }if(p.command===p.plugin+':http.body')return routes.body(p.plugin,p.arguments ?? {});if(p.command===p.plugin+':http'){const result=await routes.request(p.plugin,p.arguments??{});return metadata.get(p.plugin)?.name==='dshmarket'?workbench.marketProjection(p.arguments??{},result,p.nativeInventory,p.nativeManagedInstall):result}const command=commands.get(p.command);if(!command||command.owner!==p.plugin)throw Object.assign(Error('unknown command'),{code:-32601});return await command.value(p.arguments ?? {})})),
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
 return [kind||'Error',...(['dsh','node'].includes(error?.incompatibleEngine)?['Unsupported engine: '+error.incompatibleEngine]:[]),...(moduleName?['Missing package: '+moduleName.slice(0,128)]:[]),...(missing.length?['Missing services: '+missing.join(', ')]:[]),...frames].join('\n').slice(0,2048);
}
const write=o=>process.stdout.write(JSON.stringify(o)+'\n');
write({protocol:'dunlin.worker',major:1,nonce:process.env.DUNLIN_WORKER_NONCE,methods:Object.keys(methods),runtime:{kind:'node-dsh',node:process.versions.node,dsh:pin.dsh,commit:pin.commit,cordis:pin.cordis}});
process.on('unhandledRejection',()=>process.exit(71));process.on('uncaughtException',()=>process.exit(72));
for await(const line of readline.createInterface({input:process.stdin,crlfDelay:Infinity})){
 if(Buffer.byteLength(line)>1048576)process.exit(73);
 let q;try{q=JSON.parse(line);if(q.jsonrpc!=='2.0'||!Number.isSafeInteger(q.id)||!Object.hasOwn(methods,q.method))throw Object.assign(Error('invalid method'),{code:-32601});const result=await native.run({rpcId:q.id,nativeCall:q.params?.nativeCall},()=>methods[q.method](q.params));write({jsonrpc:'2.0',id:q.id,result:result ?? null})}
 catch(error){write({jsonrpc:'2.0',id:q?.id ?? null,error:{code:Number.isInteger(error.code)?error.code:-32603,message:error.code===-32601?'Method unavailable':'Plugin execution failed',diagnostic:diagnostic(error)}})}
}
await root.fiber.dispose();
