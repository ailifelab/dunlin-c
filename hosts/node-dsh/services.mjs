// Optional compatibility composition. Uses the actual fixed upstream service
// implementations; no model run or command approval is granted at startup.
import path from 'node:path';
import {readNativeSource,nativeHistoryEvents,nativeRestoreOptions} from './native_sessions.mjs';
import {installNativeAgents} from './native_agents.mjs';
import {installNativeReplays} from './native_replays.mjs';
import {nativeLLM} from './native_llm.mjs';
import {refreshNativeLive} from './native_live.mjs';
import {installNativeQuery} from './native_query.mjs';
import {mkdir,stat,realpath} from 'node:fs/promises';
import {AsyncLocalStorage} from 'node:async_hooks';
import os from 'node:os';
import crypto from 'node:crypto';
import {Service} from '@deepseek-ai/cordis';
import {Loader} from '@deepseek-ai/cordis-plugin-loader';
import * as SessionPersistence from '@deepseek-ai/dsh-session-persistence-jsonl';
import {WorkspaceRegistry} from '@deepseek-ai/dsh-workspace';
import {SessionTitleService} from '@deepseek-ai/dsh-session-title';
import {SystemPrompt,renderPrompt,renderContextSnapshot} from '@deepseek-ai/dsh-system-prompt';
import {SessionStore} from '@deepseek-ai/dsh-session';
import {SessionProjectionRegistry} from '@deepseek-ai/dsh-session-projection';
import {ToolRuntime} from '@deepseek-ai/dsh-tools';
import {HarnessError} from '@deepseek-ai/dsh-llm';
import LocalAttachmentProvider from '@deepseek-ai/dsh-attachment-local';
import {LocalCredentialProvider} from '@deepseek-ai/dsh-credentials-local';
import {Storage} from '@deepseek-ai/dsh-storage';
import * as JsonStorage from '@deepseek-ai/dsh-storage-json';
import * as StorageDomain from '@deepseek-ai/dsh-storage-domain';
import {CommandRuntime} from '@deepseek-ai/dsh-commands';

export async function composeServices(root,bindings){
 const home=process.env.DSH_HOME || path.join(os.homedir(),'.dsh');await mkdir(home,{recursive:true,mode:0o700});process.env.DSH_HOME=home;
 const logger=Object.assign(()=>logger,{info:(...args)=>console.error(...args),warn:(...args)=>console.error(...args),error:(...args)=>console.error(...args),debug:()=>{}});
 root.provide('logger',logger);
 new SystemPrompt(root,{includeHarnessIdentity:false});
 new SessionStore(root);
 if(bindings.native?.available){
  root.provide('llm',nativeLLM(bindings.native));
  root.provide('agentDefaultModel',{currentSelection(){
   const model=projected.get(current.getStore()?.session?.id)?.source?.model;
   if(!model)throw Error('Select a model in the current Dunlin chat first');
   return {provider:'dunlin-native',model};
  }});
 }
 new SessionProjectionRegistry(root);
 const current=new AsyncLocalStorage(),projected=new Map();
 const nativeWorkspace=()=>{if(!bindings.native?.available)return undefined;const agent=current.getStore(),entry=projected.get(agent?.session?.id);if(!entry||!bindings.native.context()?.nativeCall)throw Object.assign(Error('A current native workspace is required'),{code:-32602});const checked=bindings.native.call('session.workspace',{chat:entry.session.id}),workspace=checked?.data;if(!checked?.ok||!workspace?.available||workspace.path!==entry.path||workspace.device!==entry.device||workspace.inode!==entry.inode)throw Object.assign(Error('Native workspace changed or is unavailable'),{code:-32602});return entry};
 root.provide('dunlinNativeContext',Object.freeze({workspace:()=>nativeWorkspace()?.path,workspaces:()=>{const entry=nativeWorkspace();return entry?[{workspaceId:'native-session:'+entry.session.id,title:'当前聊天工作区',path:entry.path,sessionIds:[entry.session.id]}]:undefined}}));
 class PluginCommands extends CommandRuntime {
  register(definition){
   const remove=super.register(definition),command='slash:'+definition.name;
   let key,contribution;
   try{
    key=bindings.owned(this.ctx,command,async args=>{
     const agent=current.getStore();if(!agent)throw Object.assign(Error('Select a native chat first'),{code:-32602});
     if(typeof args.input!=='string'||args.input.length>16384)throw Object.assign(Error('Command input required'),{code:-32602});
     return this.execute(agent,'/'+definition.name+' '+args.input,[],AbortSignal.timeout(25000));
    },bindings.commands);
    contribution=bindings.owned(this.ctx,command,{kind:'command',title:'/'+definition.name,command},bindings.contributions);
   }catch(error){remove();throw error}
   let removed=false;return()=>{if(removed)return;removed=true;remove();bindings.commands.delete(key);bindings.contributions.delete(contribution)};
  }
 }
 new PluginCommands(root);
 new Loader(root,{baseUrl:new URL("file://"+home+"/").href});
 const executionFault=new AsyncLocalStorage();
 class PluginTools extends ToolRuntime {
  register(definition){
   const execute=definition.execute;
   const remove=super.register({...definition,async execute(...args){
    try{return await execute.apply(this,args)}catch(error){
     if(!(error instanceof HarnessError)&&error?.name!=='AbortError'){const marker=executionFault.getStore();if(marker)marker.error=error}
     throw error;
    }
   }});
   const command='tool:'+definition.name;
   let key,contribution;
   try{
    key=bindings.owned(this.ctx,command,async args=>{const marker={};const result=await executionFault.run(marker,()=>this.execute({callId:crypto.randomUUID(),name:definition.name,arguments:args,agent:current.getStore(),signal:AbortSignal.timeout(25000)}));if(marker.error)throw marker.error;return result},bindings.commands);
    contribution=bindings.owned(this.ctx,command,{kind:'command',title:definition.name,command},bindings.contributions);
   }catch(error){remove();throw error}
   let removed=false;return ()=>{if(removed)return;removed=true;remove();bindings.commands.delete(key);bindings.contributions.delete(contribution)};
  }
 }
 const pluginTools=new PluginTools(root,{mode:'native'});
 new LocalAttachmentProvider(root,{dshHome:home});
 await root.plugin(LocalCredentialProvider,{dshHome:home,path:path.join(home,'credentials.yaml')});
 new Storage(root);
 await root.plugin(JsonStorage,{root:path.join(home,'storage')});
 await root.plugin(StorageDomain,{backend:'json'});
 // This is an explicitly derived, per-host projection. Native C history is
 // never recovered or overwritten from this cache.
 await root.plugin(SessionPersistence,{root:path.join(home,'sessions','native-'+process.env.DUNLIN_WORKER_NONCE)});
 await root.plugin(WorkspaceRegistry);
 class NativeTitles extends SessionTitleService {rename(session,title){if(bindings.native?.available&&projected.has(session.id)){const result=bindings.native.call('session.rename',{chat:session.id,title});if(!result?.ok)throw Object.assign(Error(result?.error??'Native rename refused'),{code:-32602})}return super.rename(session,title)}}
 await root.plugin(NativeTitles,{fallbackMaxWords:12,fallbackMaxBytes:160,maxTitleBytes:512});
 // A DSH plugin may request a preset, but this adapter cannot grant native
 // model/tool authority. Escalation is refused, never silently approved.
 class NativePermissions extends Service {
  constructor(ctx){super(ctx,'permissionPresets')}
  set(session,preset){
   if(preset!=='read-only')throw Object.assign(new Error('Native permission escalation requires explicit approval'),{code:'DUNLIN_APPROVAL_REQUIRED'});
   return undefined;
  }
 }
 new NativePermissions(root);
 function refresh(entry,source){
  const admission=bindings.native.context()?.nativeCall;if(!source&&admission&&entry.refreshedCall===admission)return;
  source??=readNativeSource(bindings.native,entry.session.id);const session=entry.session;
  if(source&&entry.source?.revision!==source.revision){
   const previous=entry.source?.state.messages??[];
   if(source.state.messages.length<previous.length||previous.some((row,index)=>JSON.stringify(row)!==JSON.stringify(source.state.messages[index])))throw Object.assign(Error('Canonical history changed; native projection must be rebound'),{code:-32602});
   const events=nativeHistoryEvents(entry.session.id,source),before=nativeHistoryEvents(entry.session.id,entry.source);
   // Positions closing a live tail may differ. Refuse a false append rather
   // than fabricate a completed turn or replace canonical C source.
   if(events.length<before.length||before.some((event,index)=>JSON.stringify(event)!==JSON.stringify(events[index])))throw Object.assign(Error('Canonical live boundary changed; refresh required'),{code:-32602});
   for(const event of events.slice(before.length)){
    const appended=session.append(event.type,event.data,{...(event.surfaceOp?{surfaceOp:event.surfaceOp}:{}),[Symbol.for('dunlin.native.sourceTime')]:event.time});
    if(appended.time!==event.time)throw Object.assign(Error('SDK did not retain native source time'),{code:-32602});
   }
  }
  if(source){
   const title=session.snapshotEvents().findLast(event=>event.type==='session/title')?.data.title;
   if(source.title&&title!==source.title)session.append('session/title',{title:source.title,messageSeqs:[],source:{kind:'user'}});
   if(source.model){const selected={provider:'dunlin-native',model:source.model};const previous=session.snapshotEvents().findLast(event=>event.type==='model/selection')?.data;
    if(JSON.stringify(previous)!==JSON.stringify(selected))session.append('model/selection',selected);entry.selection=selected;
   }
   entry.source=source;
  }
  if(bindings.native?.available&&source)refreshNativeLive(root,bindings.native,entry);
  entry.refreshedCall=admission;
 }
 async function adopt(context,options){

  if(typeof context.chat!=='string'||!context.chat||context.chat.length>64||typeof context.path!=='string'||!path.isAbsolute(context.path))throw Object.assign(Error('Invalid native context'),{code:-32602});
  const canonical=await realpath(context.path),st=await stat(canonical,{bigint:true});
  if(canonical!==context.path||!st.isDirectory()||String(st.dev)!==context.device||String(st.ino)!==context.inode)throw Object.assign(Error('Native workspace changed'),{code:-32602});
  if(options?.meta?.cwd!==undefined&&options.meta.cwd!==canonical)throw Object.assign(Error('Native SDK metadata cwd differs from the validated workspace'),{code:-32602});
  let entry=projected.get(context.chat);
  if(entry&&entry.path!==canonical){entry.detach();projected.delete(context.chat);entry=undefined}
  const source=options?.nativeSource??(bindings.native?.available?readNativeSource(bindings.native,context.chat):null);
  let session=entry?.session;
  if(!session){
   if(root.sessions.get(context.chat))throw Object.assign(Error('Session identity conflict'),{code:-32602});
   const restored=source?nativeRestoreOptions(context.chat,source):{};
   const seed=options?.seed??restored.seed,inherited=options?.inheritedEventCount??restored.inheritedEventCount;
   session=root.sessions.prepare(context.chat,{meta:{...restored.meta,...options?.meta,cwd:canonical},...(seed?{seed}:{}),...(inherited===undefined?{}:{inheritedEventCount:inherited})});
   const writer=await root.sessionPersistence.create(session.header,{inheritedEventCount:session.inheritedEventCount});
   try{await writer.append(session.snapshotEvents());await writer.flush()}catch(error){await writer.close();throw error}
   const detach=root.sessions.enter(session);root.sessions.announce(session);
   entry={session,detach,path:canonical,device:context.device,inode:context.inode,position:null,writer,source,live:true};projected.set(context.chat,entry);
   root.effect(()=>async()=>{detach();await writer.close()});
   await root.sessions.flush(session);const workspace=root.workspaceRegistry.list().find(w=>w.path===canonical);if(workspace)await workspace.attachSession(context.chat);
  }
  // Only a C-reported completed model position supplies a checkpoint boundary.
  // This is an optional projection; it does not replace or rewind the C journal.
  const pos=context.position;
  if(!source&&pos&&Number.isSafeInteger(pos.turn)&&pos.turn>0&&Number.isSafeInteger(pos.step)&&pos.step>0&&entry.position!==JSON.stringify(pos)){
   session.append('turn/start',{turn:pos.turn});session.append('step/start',pos);
   session.append('step/end',pos);session.append('turn/end',{turn:pos.turn});entry.position=JSON.stringify(pos);
  }
  if(source)refresh(entry,source);await root.sessions.flush(session);return entry;
 }
 const coldFlights=new Map();
 async function hydrate(id,knownSource){
  const existing=projected.get(id);if(existing){refresh(existing);await root.sessions.flush(existing.session);return existing}
  if(coldFlights.has(id))return coldFlights.get(id);
  const flight=(async()=>{const source=knownSource??readNativeSource(bindings.native,id);if(!source.available)throw Object.assign(Error('Native workspace unavailable'),{code:-32602});const entry=await adopt({chat:id,path:source.cwd,device:source.device,inode:source.inode},{...nativeRestoreOptions(id,source),nativeSource:source});entry.live=false;return entry})();coldFlights.set(id,flight);
  try{return await flight}finally{coldFlights.delete(id)}
 }
 function enterReplay(parent,id,source,session){
  const entry={session,path:parent.path,device:parent.device,inode:parent.inode,position:null,source,live:false,writer:null,detach:null};
  const append=session.append.bind(session);session.append=(type,data,settings)=>{
   if(type==='user/message'&&data?.source?.kind==='dsh-checkpoint-rewind'&&data.source.form==='notice'){
    const result=bindings.native.call('session.notice',{chat:id,revision:entry.source.revision,message:data});if(!result?.ok)throw Object.assign(Error(result?.error??'Native replay annotation refused'),{code:-32602});entry.source=readNativeSource(bindings.native,id);
   }
   return append(type,data,settings);
  };
  entry.writerReady=(async()=>{const writer=await root.sessionPersistence.create(session.header,{inheritedEventCount:session.inheritedEventCount});entry.writer=writer;try{await writer.append(session.snapshotEvents());await writer.flush()}catch(error){await writer.close();throw error}})();entry.writerReady.catch(()=>{});
  entry.detach=root.sessions.enter(session);projected.set(id,entry);
  root.effect(()=>async()=>{entry.detach();try{await entry.writerReady}finally{if(entry.writer)await entry.writer.close()}});root.sessions.announce(session);return session;
 }
 if(bindings.native?.available)installNativeReplays(root,{native:bindings.native,projected,refresh,enterReplay});
 const nativeAgents=bindings.native?.available?installNativeAgents(root,{native:bindings.native,projected,adopt,refresh,hydrate}):null;
 if(nativeAgents)installNativeQuery(root,{native:bindings.native});
 if(nativeAgents){const backend=root.sessionPersistence,open=backend.open.bind(backend),stat=backend.stat.bind(backend);
  backend.open=async(id,access,options)=>{if(access==='read')await hydrate(id);return open(id,access,options)};
  backend.stat=async(id,options)=>{await hydrate(id);return stat(id,options)};
 }
 return {home,async observe(id){
  // Hydration reads through C's current package/root ownership fence, even if
  // another surface has a cached SDK session with the same requested id.
  if(!nativeAgents)return;
  const source=readNativeSource(bindings.native,id);
  const entry=projected.get(id);
  if(entry){refresh(entry,source);await root.sessions.flush(entry.session)}
  else await hydrate(id,source);
 },prompt(args){
  const agent=current.getStore();
  if(!nativeAgents||!agent||args?.mode!=='queue'||!Array.isArray(args.content)||!args.content.length||args.content.some(block=>block?.type!=='text'||typeof block.text!=='string'))throw Object.assign(Error('Native text queue prompt required'),{code:-32602});
  const text=args.content.map(block=>block.text).join('');if(!text.trim()||Buffer.byteLength(text)>16384)throw Object.assign(Error('Prompt must be 1..16384 bytes'),{code:-32602});
  const messageId=crypto.randomUUID();agent.followup({id:messageId,role:'user',content:[{type:'text',text}],source:{kind:'user'}});
  return {status:200,body:{ok:true,value:{accepted:true,messageId}}};
 },async context(){
  const agent=current.getStore();if(!agent)throw Object.assign(Error('Native context required'),{code:-32602});
  const assembly=await root.systemPrompt.assemble({agent});const text=[renderPrompt(assembly),renderContextSnapshot(assembly)].filter(Boolean).join('\n\n');if(Buffer.byteLength(text)>65536)throw Object.assign(Error('Extension context exceeds 64 KiB'),{code:-32602});return {text};
 },tools(){
  const schemas=new Map(pluginTools.schemas().map(schema=>[schema.name,schema]));
  return [...bindings.commands].filter(([key])=>key.includes(':tool:')).map(([key,row])=>({plugin:row.owner,command:key,schema:schemas.get(key.slice(key.indexOf(':tool:')+6))})).filter(row=>row.schema);
 },async invoke(context,action){
  if(!context)return current.run(undefined,action);
  const entry=await adopt(context);entry.live=true;
  const agent=nativeAgents?nativeAgents.proxy(entry):Object.freeze({session:entry.session,ctx:root,tools:root.tools});
  try{return await current.run(agent,action)}finally{for(const row of projected.values()){await row.writerReady;await root.sessions.flush(row.session)}}
 }};
}
