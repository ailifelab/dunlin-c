// Optional C admission transport. This never runs a model or holds a model key.
// SDK agent methods can be synchronous, so their durable C admission uses its
// own blocking FD, separate from the asynchronous stdin/stdout RPC reader.
import fs from 'node:fs';
import {AsyncLocalStorage} from 'node:async_hooks';
const LIMIT=1048576;
export function nativeBridge({fd,nonce}={fd:process.env.DUNLIN_NATIVE_FD===undefined?undefined:Number(process.env.DUNLIN_NATIVE_FD),nonce:process.env.DUNLIN_WORKER_NONCE}){
 if(fd!==undefined&&(!Number.isInteger(fd)||fd!==3||!/^[a-f0-9]{64}$/.test(nonce??'')))throw Error('Invalid native admission channel');
 const local=new AsyncLocalStorage();let active,sequence=0,buffer=Buffer.alloc(0);
 const available=fd!==undefined;
 const read=()=>{for(;;){const at=buffer.indexOf(10);if(at>=0){const line=buffer.subarray(0,at);buffer=buffer.subarray(at+1);return JSON.parse(line.toString('utf8'))}if(buffer.length>=LIMIT)throw Error('Native reply exceeds limit');const chunk=Buffer.alloc(Math.min(65536,LIMIT-buffer.length));let n;try{n=fs.readSync(fd,chunk,0,chunk.length,null)}catch(error){if(error.code==='EINTR')continue;throw error}if(!n)throw Error('Native admission channel closed');buffer=Buffer.concat([buffer,chunk.subarray(0,n)])}};
 const send=frame=>{const bytes=Buffer.from(JSON.stringify(frame)+'\n');if(bytes.length>LIMIT)throw Object.assign(Error('Native request exceeds limit'),{code:-32602});for(let offset=0;offset<bytes.length;){let n;try{n=fs.writeSync(fd,bytes,offset,bytes.length-offset)}catch(error){if(error.code==='EINTR')continue;throw error}if(!n)throw Error('Native admission write failed');offset+=n}};
 return Object.freeze({available,context:()=>local.getStore(),async run(rpc,action){const previous=active;active=rpc;try{return await local.run(rpc,action)}finally{active=previous}},call(method,args={}){
  if(!available||(!active&&!local.getStore()?.nativeCall)||(active&&!Number.isSafeInteger(active.rpcId))||typeof method!=='string'||!/^[A-Za-z0-9_.-]{1,128}$/.test(method)||!args||Array.isArray(args)||typeof args!=='object')throw Object.assign(Error('Native admission unavailable'),{code:-32602});
  let rpcId=active?.rpcId;
  if(!rpcId){const wake=++sequence;send({protocol:'dunlin.native.wake',major:1,nonce,sequence:wake,rpcId:0});const ready=read();if(ready.protocol!=='dunlin.native.ready'||ready.major!==1||ready.nonce!==nonce||ready.sequence!==wake||!Number.isSafeInteger(ready.rpcId)||ready.rpcId<=0)throw Error('Native wake reply mismatch');rpcId=ready.rpcId}
  const frame={protocol:'dunlin.native.call',major:1,nonce,sequence:++sequence,rpcId,method,arguments:args,...(local.getStore()?.nativeCall?{nativeCall:local.getStore().nativeCall}:{})};send(frame);
  const response=read();if(response.protocol!=='dunlin.native.reply'||response.major!==1||response.nonce!==nonce||response.sequence!==sequence||response.rpcId!==rpcId||!Object.hasOwn(response,'result'))throw Error('Native admission reply mismatch');return response.result;
 }});
}
