// Owned protocol fixture. No model, user configuration or external service.
import fs from 'node:fs';import readline from 'node:readline';import {pathToFileURL} from 'node:url';
const {nativeBridge}=await import(pathToFileURL(process.argv[2]));const bridge=nativeBridge();const nonce=process.env.DUNLIN_WORKER_NONCE,fd=Number(process.env.DUNLIN_NATIVE_FD);let seq=0,deferred;
const write=value=>process.stdout.write(JSON.stringify(value)+'\n');
const packet=(q,overrides={})=>({protocol:'dunlin.native.call',major:1,nonce,sequence:++seq,rpcId:q.id,method:'fixture.inspect',arguments:{text:'本地 C admission'},nativeCall:'owned-call',...overrides});
const reply=()=>{const bytes=[];for(;;){const b=Buffer.alloc(1);if(!fs.readSync(fd,b))throw Error('closed');if(b[0]===10)return JSON.parse(Buffer.concat(bytes).toString('utf8'));bytes.push(b)}};
write({protocol:'dunlin.worker',major:1,nonce,methods:['test','ping'],runtime:{kind:'owned-native-fixture',pid:process.pid}});
for await(const line of readline.createInterface({input:process.stdin,crlfDelay:Infinity})){
 const q=JSON.parse(line);
 try{
  const value=await bridge.run({rpcId:q.id,nativeCall:'owned-call'},async()=>{
   const mode=q.params.mode;
   if(q.method==='ping')return {deferred};
   if(mode==='normal'||mode==='refused'||mode==='invalidResult'||mode==='resultTooLarge'||mode==='cancelAfter'||mode==='killAfter')return bridge.call(mode==='refused'?'fixture.forbidden':'fixture.inspect',{text:'本地 C admission',mode});
   if(mode==='deferred'){setTimeout(()=>{deferred=bridge.call('fixture.inspect',{text:'owned deferred admission'})},20);return {scheduled:true}}
   if(mode==='flood'){for(let i=0;i<129;i++)bridge.call('fixture.inspect',{});return {unexpected:true}}
   let frame=packet(q);
   if(mode==='nonce')frame.nonce='0'.repeat(64);
   if(mode==='nulNonce')frame.nonce=nonce+'\0suffix';
   if(mode==='rpc')frame.rpcId=q.id-1;
   if(mode==='sequence')frame.sequence=0;
   if(mode==='partial'){fs.writeSync(fd,JSON.stringify(frame).slice(0,35));await new Promise(()=>{});return {unexpected:true}}
   if(mode==='oversize'){fs.writeSync(fd,Buffer.alloc(1048592,120));await new Promise(()=>{});return {unexpected:true}}
   const bytes=Buffer.from(JSON.stringify(frame)+'\n');
   if(mode==='fragmented'){for(let i=0;i<bytes.length;i+=17){fs.writeSync(fd,bytes.subarray(i,i+17));await new Promise(r=>setTimeout(r,1))}return reply().result}
   fs.writeSync(fd,bytes);reply();return {unexpected:true};
  });write({jsonrpc:'2.0',id:q.id,result:value});
 }catch(error){write({jsonrpc:'2.0',id:q.id,error:{code:Number.isInteger(error.code)?error.code:-32603,message:'Owned fixture failure'}})}
}
