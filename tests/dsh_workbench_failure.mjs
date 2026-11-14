// Actual upstream SideChat WebSocket + canonical C jobs. No transcript HTTP
// refresh is allowed between the model finishing and its invalidation notice.
import fs from 'node:fs/promises';
import path from 'node:path';
import {spawn} from 'node:child_process';
import readline from 'node:readline';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
const dir=process.env.DUNLIN_WORKBENCH_REPORT;await fs.mkdir(dir,{recursive:true});
const child=spawn(path.resolve('build/v51/current-core/python-asan'),['tests/dsh_workbench_owner.py',path.resolve(process.env.DUNLIN_WORKBENCH_LIBRARY),path.resolve(process.env.DUNLIN_WORKBENCH_ASSETS),'reports/v51-current/real-dsh-published-inputs.json'],{stdio:['pipe','pipe','pipe'],env:{...process.env,DUNLIN_WORKBENCH_NATIVE_AGENTS:'1',DUNLIN_WORKBENCH_PROJECTION:'1'}});
let stderr='';child.stderr.on('data',x=>stderr+=x);const reader=readline.createInterface({input:child.stdout});const init=await new Promise((resolve,reject)=>{reader.once('line',x=>resolve(JSON.parse(x)));child.once('exit',code=>reject(Error('owner exited '+code+' '+stderr)))});
const pause=ms=>new Promise(r=>setTimeout(r,ms));
async function call(route,data={}){const r=await(await fetch('http://127.0.0.1:'+init.port+route,{method:'POST',headers:{'X-Dunlin-Owner':init.token},body:JSON.stringify(data)})).json();if(!r.ok)throw Object.assign(Error(r.error),{failure:r.failure});return r.value}
const command=(command,args)=>call('/invoke',{plugin:'dsh-better-sidebar',command,args});
async function api(method,args){let r=await command('http',{url:'/sidebar/api/'+method,method:'POST',body:args});if(r.pendingHandle){const id=r.pendingHandle;try{for(let i=0;i<300;i++){const next=await command('http.pending',{id});if(next.ready){r=next.response;break}await pause(50)}}finally{await command('http.pending',{id,release:true})}}assert.equal(r.status,200,JSON.stringify(r));assert.equal(r.body.ok,true,JSON.stringify(r));return r.body.value}
const results=[];
try{
 const {childId}=await api('sidechat.start',{sessionId:'ui-chat'});
 await call('/native-text-fixture',{fail:true});
 await api('sidechat.prompt',{childId,text:'Owned original failed provider turn'});
 const failed=await call('/native-agents-run');assert.equal(failed.runs.length,1);assert.equal(failed.runs[0].run.status,'failed');
 const transcript=await api('sidechat.events',{childId});
 await fs.writeFile(path.join(dir,'failed-transcript.json'),JSON.stringify(transcript,null,2));
 const errors=transcript.events.filter(e=>e.type==='turn/end'&&e.data.reason?.kind==='error');
 assert.equal(errors.length,1,'actual C provider failure was presented as completed or silently omitted');
 assert.match(errors[0].data.reason.error.message,/503/);
 const state=await call('/native-state-fixture',{chat:childId});assert.equal(state._dunlin_turn_outcomes.length,1);assert.equal(state._dunlin_turn_outcomes[0].status,1);
 const fork=await command('native.session.fork',{sessionId:childId});const forkId=fork.value.id;
 const copied=await call('/native-state-fixture',{chat:forkId});assert.deepEqual(copied._dunlin_turn_outcomes,state._dunlin_turn_outcomes,'native fork discarded actual failure outcome');assert.deepEqual(copied.messages,state.messages,'outcome copying changed canonical messages');
 await call('/native-text-fixture',{});
 await api('sidechat.prompt',{childId,text:'Owned explicit continuation after failed provider'});
 const recovered=await call('/native-agents-run');assert.equal(recovered.runs.length,1);assert.equal(recovered.runs[0].run.status,'completed');
 const after=await api('sidechat.events',{childId});
 assert.equal(after.events.filter(e=>e.type==='turn/end'&&e.data.reason?.kind==='error').length,1,'new successful turn erased the prior failure');
 assert.equal(after.events.filter(e=>e.type==='assistant/message').length,1,'continuation missing genuine assistant answer');
 await fs.writeFile(path.join(dir,'continued-transcript.json'),JSON.stringify(after,null,2));
 await call('/native-runtime-restart');
 const cold=await api('sidechat.events',{childId});
 assert.equal(cold.events.filter(e=>e.type==='turn/end'&&e.data.reason?.kind==='error').length,1,'cold SDK reconstruction erased actual C outcome');
 assert.equal((await call('/native-agent-jobs')).modelRequests,3,'failure observation or cold reconstruction replayed model');
 results.push({nativeForkCopiesFailedOutcome:true,originalMessagesUnchanged:true,failedProviderVisible:true,explicitContinuationKeepsPriorFailure:true,coldFailureHistory:true,noReplay:true});
 console.log('PASS C failure outcome -> original transcript error, continuation and cold reconstruction retain it');
}finally{
 child.stdin.end('\n');await new Promise(resolve=>child.once('exit',resolve));
 await fs.writeFile(path.join(dir,'results.json'),JSON.stringify(results,null,2));await fs.writeFile(path.join(dir,'owner-stderr.log'),stderr);
 await fs.writeFile(path.join(dir,'test-inputs.json'),JSON.stringify({library:process.env.DUNLIN_WORKBENCH_LIBRARY,assets:process.env.DUNLIN_WORKBENCH_ASSETS,testSHA256:createHash('sha256').update(await fs.readFile('tests/dsh_workbench_failure.mjs')).digest('hex'),ownerSHA256:createHash('sha256').update(await fs.readFile('tests/dsh_workbench_owner.py')).digest('hex')},null,2));
}
