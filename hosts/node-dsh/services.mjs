// Optional compatibility composition. Uses the actual fixed upstream service
// implementations; no model run or command approval is granted at startup.
import path from 'node:path';
import {mkdir,stat,realpath} from 'node:fs/promises';
import {AsyncLocalStorage} from 'node:async_hooks';
import os from 'node:os';
import crypto from 'node:crypto';
import {Service} from '@deepseek-ai/cordis';
import {Loader} from '@deepseek-ai/cordis-plugin-loader';
import * as SessionPersistence from '@deepseek-ai/dsh-session-persistence-jsonl';
import {WorkspaceRegistry} from '@deepseek-ai/dsh-workspace';
import {SessionTitleService} from '@deepseek-ai/dsh-session-title';
import {SystemPrompt} from '@deepseek-ai/dsh-system-prompt';
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
 new SessionProjectionRegistry(root);
 const current=new AsyncLocalStorage(),projected=new Map();
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
 new LocalCredentialProvider(root,{dshHome:home,path:path.join(home,'credentials.yaml')});
 new Storage(root);
 await root.plugin(JsonStorage,{root:path.join(home,'storage')});
 await root.plugin(StorageDomain,{backend:'json'});
 await root.plugin(SessionPersistence,{root:path.join(home,'sessions')});
 await root.plugin(WorkspaceRegistry);
 await root.plugin(SessionTitleService,{fallbackMaxWords:12,fallbackMaxBytes:160,maxTitleBytes:512});
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
 return {home,tools(){
  const schemas=new Map(pluginTools.schemas().map(schema=>[schema.name,schema]));
  return [...bindings.commands].filter(([key])=>key.includes(':tool:')).map(([key,row])=>({plugin:row.owner,command:key,schema:schemas.get(key.slice(key.indexOf(':tool:')+6))})).filter(row=>row.schema);
 },async invoke(context,action){
  if(!context)return current.run(undefined,action);
  if(typeof context.chat!=='string'||!context.chat||context.chat.length>64||typeof context.path!=='string'||!path.isAbsolute(context.path))throw Object.assign(Error('Invalid native context'),{code:-32602});
  const canonical=await realpath(context.path),st=await stat(canonical,{bigint:true});
  if(canonical!==context.path||!st.isDirectory()||String(st.dev)!==context.device||String(st.ino)!==context.inode)throw Object.assign(Error('Native workspace changed'),{code:-32602});
  let entry=projected.get(context.chat);
  if(entry&&entry.path!==canonical){entry.detach();projected.delete(context.chat);entry=undefined}
  let session=entry?.session;
  if(!session){
   if(root.sessions.get(context.chat))throw Object.assign(Error('Session identity conflict'),{code:-32602});
   session=root.sessions.prepare(context.chat,{meta:{cwd:canonical}});
   const detach=root.sessions.enter(session);root.sessions.announce(session);
   entry={session,detach,path:canonical,position:null};projected.set(context.chat,entry);
  }
  // Only a C-reported completed model position supplies a checkpoint boundary.
  // This is an optional projection; it does not replace or rewind the C journal.
  const pos=context.position;
  if(pos&&Number.isSafeInteger(pos.turn)&&pos.turn>0&&Number.isSafeInteger(pos.step)&&pos.step>0&&entry.position!==JSON.stringify(pos)){
   session.append('turn/start',{turn:pos.turn});session.append('step/start',pos);
   session.append('step/end',pos);session.append('turn/end',{turn:pos.turn});entry.position=JSON.stringify(pos);
  }
  const agent=Object.freeze({session,ctx:root,tools:root.tools});
  return current.run(agent,action);
 }};
}
