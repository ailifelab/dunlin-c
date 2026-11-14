// Synchronous SessionStore seed/fork face; C alone owns the canonical prefix.
import crypto from 'node:crypto';
import {isDeepStrictEqual} from 'node:util';
import {readNativeSource,nativeHistoryEvents} from './native_sessions.mjs';
const fail=message=>Object.assign(Error(message),{code:-32602});
const canonicalTypes=new Set(['turn/start','turn/end','step/start','step/end','user/message','assistant/message','tool/call','tool/result','system/message','developer/message']);
function semantic(events){return events.filter(e=>canonicalTypes.has(e.type)).map(event=>{const data=structuredClone(event.data);if(event.type==='user/message'){delete data.id;if(data.source?.kind==='dsh-checkpoint-rewind')data.source.kind='plugin:dsh-checkpoint-rewind'}if(data.message)delete data.message.id;return {type:event.type,data}})}
export function installNativeReplays(root,{native,projected,refresh,enterReplay}){
 const original=root.sessions.create;
 root.sessions.create=function(id,options){
  const parent=projected.get(options?.meta?.parentSession),seed=options?.seed,count=options?.inheritedEventCount;
  if(!parent||root.sessions.get(parent.session.id)!==parent.session||!Array.isArray(seed)||!Number.isSafeInteger(count)||count<0||count>seed.length||seed.length-count>1||options?.meta?.isSeeded!==true)throw fail('Native replay requires an exact parent seed');
  if(options.meta?.cwd!==undefined&&options.meta.cwd!==parent.path)throw fail('Native replay metadata cannot change the validated workspace');
  const prefix=seed.slice(0,count);if(seed.length>count&&!isDeepStrictEqual(seed[count],{type:'session/end-seed',seq:count,time:prefix.at(-1)?.time??0,data:{inherited:true}}))throw fail('Native replay has synthetic execution closers');
  refresh(parent);const live=parent.session.snapshotEvents();if(count>live.length||!isDeepStrictEqual(prefix,live.slice(0,count)))throw fail('Native replay seed is not the live source prefix');
  const boundaries=new Map(),events=nativeHistoryEvents(parent.session.id,parent.source,boundaries),last=prefix.at(-1);let through=-1;
  if(count){if(last.type!=='turn/end'||!Number.isSafeInteger(last.data.turn)||!boundaries.has(last.data.turn))throw fail('Native replay requires a closed canonical turn');through=boundaries.get(last.data.turn);const end=events.findIndex(e=>e.type==='turn/end'&&e.data.turn===last.data.turn);if(end<0||!isDeepStrictEqual(semantic(prefix),semantic(events.slice(0,end+1))))throw fail('Native replay seed differs from canonical history')}
  const child=id??crypto.randomUUID();if(typeof child!=='string'||!child||child.length>64||!/^[A-Za-z0-9_-]+$/.test(child)||root.sessions.get(child))throw fail('Invalid native replay identity');
  const prepared=root.sessions.prepare(child,{seed,meta:{cwd:parent.path,parentSession:parent.session.id,isSeeded:true},inheritedEventCount:count});
  const metadata={parentSession:parent.session.id,isSeeded:true,checkpointReplay:true};const result=native.call('session.replay',{parent:parent.session.id,chat:child,revision:parent.source.revision,through,metadata});if(!result?.ok)throw fail(result?.error??'Native replay refused');
  const source=readNativeSource(native,child);if(!source.available||source.cwd!==parent.path||source.device!==parent.device||source.inode!==parent.inode)throw fail('Native replay workspace changed');
  return enterReplay(parent,child,source,prepared);
 };
 root.effect(()=>()=>{root.sessions.create=original});
}
