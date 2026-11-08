import {AsyncLocalStorage} from 'node:async_hooks';
import {randomUUID} from 'node:crypto';
// A suspended real plugin operation, never replayed after an approval. C retains
// its invocation audit while the native user decides. No listening port.
export function installQuestions(root){
 const current=new AsyncLocalStorage(),pending=new Map();
 root.provide('userQuestions',{async ask(input){
  const operation=current.getStore();
  if(!operation||!operation.enabled)throw Error('Native confirmation unavailable (fail closed)');
  const questions=input?.questions;
  if(!Array.isArray(questions)||questions.length!==1)throw Error('Native confirmation supports one question');
  const q=questions[0];if(typeof q?.id!=='string'||typeof q.question!=='string'||q.question.length>4096||typeof q.detail!=='string'||q.detail.length>16384||!Array.isArray(q.options)||q.options.length!==2||q.options.some(o=>typeof o.label!=='string'||o.label.length>128))throw Error('Unsupported confirmation shape');
  if(operation.question)throw Error('Concurrent plugin questions unsupported');
  let answer;const answered=new Promise(resolve=>answer=resolve);
  const denied=()=>answer(false);input.signal?.addEventListener('abort',denied,{once:true});
  const timer=setTimeout(denied,60000);
  operation.id=randomUUID();operation.question={id:q.id,question:q.question,detail:q.detail,options:q.options.map(o=>({label:o.label,description:typeof o.description==='string'?o.description.slice(0,2048):''}))};operation.answer=answer;
  operation.changed();
  try{const allowed=await answered;return {answers:[{id:q.id,selected:allowed?[q.options[0].label]:[q.options[1].label]}]}}
  finally{clearTimeout(timer);input.signal?.removeEventListener('abort',denied);pending.delete(operation.id);operation.question=null;operation.answer=null}
 }});
 async function settle(operation){
  const signal=new Promise(resolve=>operation.changed=()=>resolve({question:true}));
  if(operation.question)return {nativeQuestion:operation.question,continuation:operation.id};
  const result=await Promise.race([operation.result,signal]);
  if(result.question){if(!operation.question)return settle(operation);pending.set(operation.id,operation);return {nativeQuestion:operation.question,continuation:operation.id}}
  pending.delete(operation.id);if(result.error)throw result.error;return result.value;
 }
 return {
  async run(owner,enabled,action){
   if(pending.size>=16)throw Object.assign(Error('Native confirmation capacity reached'),{code:-32602});
   const operation={id:randomUUID(),owner,enabled,question:null,answer:null,changed:()=>{}};
   operation.result=current.run(operation,()=>Promise.resolve().then(action)).then(value=>({value}),error=>({error}));
   return settle(operation);
  },
  async answer(owner,args){
   const operation=pending.get(args.id);if(!operation||operation.owner!==owner||!operation.answer||typeof args.allowed!=='boolean')throw Object.assign(Error('Stale native confirmation'),{code:-32602});
   pending.delete(args.id);const resolve=operation.answer;operation.question=null;operation.answer=null;resolve(args.allowed);return settle(operation);
  },
  cancel(owner){for(const [id,operation]of pending)if(operation.owner===owner){operation.answer?.(false);pending.delete(id)}},
 };
}
