// Structural AgentRegistry face for the pinned SDK. Execution, input admission
// and cancellation belong to the C owner; this module has no LLM provider loop.
import {Service} from '@deepseek-ai/cordis';
export function installNativeAgents(root,{native,projected,adopt,refresh,hydrate}){
 const fail=message=>Object.assign(Error(message),{code:-32602});
 const call=(method,args)=>{const result=native.call(method,args);if(!result?.ok)throw fail(result?.error??'Native agent admission failed');return result.data};
 function proxy(entry){
  if(entry.agent)return entry.agent;
  const options=Object.freeze(entry.source?.model?{provider:'dunlin-native',model:entry.source.model}:{});
  entry.selection??=options.model?{provider:options.provider,model:options.model}:undefined;
  const pending=()=>call('agent.inbox',{chat:entry.session.id});
  const inbox={get nextStep(){return pending().nextStep},get nextTurn(){return pending().nextTurn},clear(){const result=call('agent.dequeue',{chat:entry.session.id});if(result.remaining)throw fail('Native input has already been claimed');return result},remove(id){return call('agent.dequeue',{chat:entry.session.id,id}).removed>0}};
  const model={get selection(){return entry.selection},select(value){if(JSON.stringify(entry.selection)===JSON.stringify(value))return;const accepted=call('agent.model',{chat:entry.session.id,selection:value});entry.selection=accepted.selection;entry.session.append('model/selection',entry.selection)}};
  const agent={session:entry.session,ctx:root,tools:root.tools,options,model,inbox,
   get status(){return call('agent.status',{chat:entry.session.id}).status},
   followup(message){return call('agent.followup',{chat:entry.session.id,message})},
   inject(message){call('agent.inject',{chat:entry.session.id,message})},
   steer(message){call('agent.followup',{chat:entry.session.id,message,steer:true})},
   cancel(reason,options){return call('agent.cancel',{chat:entry.session.id,reason,options})},
  };
  entry.agent=Object.freeze(agent);entry.live=true;return entry.agent;
 }
 class NativeAgents extends Service {
  constructor(ctx){super(ctx,'agents')}
  get(id){const entry=projected.get(id);if(!entry||!entry.live)return undefined;refresh(entry);return proxy(entry)}
  async create(options){
   options?.signal?.throwIfAborted();const parent=options?.meta?.parentSession,entry=projected.get(parent);
   if(!entry||!Array.isArray(options.seed)||typeof options.sessionId!=='string')throw fail('Native parent/seed required');
   if(options.meta?.cwd!==undefined&&options.meta.cwd!==entry.path)throw fail('Native SDK metadata cannot change the validated workspace');refresh(entry);
   const descriptor=options.seed.findLast(event=>event.type==='subagent/descriptor');
   call('agent.create',{parent,chat:options.sessionId,revision:entry.source.revision,metadata:{...options.meta,...(descriptor?{descriptor:descriptor.data}:{})}});
   const child=await adopt({chat:options.sessionId,path:entry.path,device:entry.device,inode:entry.inode},{seed:options.seed,meta:options.meta,inheritedEventCount:options.inheritedEventCount});
   if(options.setup)await options.setup(root.extend());const agent=proxy(child);
   return {agent,async dispose(){call('agent.cancel',{chat:child.session.id,reason:{kind:'disposed'},options:{keepInbox:true}});child.live=false}};
  }
  async resume(options){
   const entry=await hydrate(options?.resumeSessionId);refresh(entry);entry.live=true;
   if(options.setup)await options.setup(root.extend());return {agent:proxy(entry),async dispose(){call('agent.cancel',{chat:entry.session.id,reason:{kind:'disposed'},options:{keepInbox:true}});entry.live=false}};
  }
 }
 const agents=new NativeAgents(root);root.provide('sessionController',{modelCatalog:async()=>call('agent.models',{})});return {agents,proxy,call};
}
