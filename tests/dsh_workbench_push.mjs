// Actual upstream SideChat WebSocket + canonical C jobs. No transcript HTTP
// refresh is allowed between the model finishing and its invalidation notice.
import fs from 'node:fs/promises';
import path from 'node:path';
import {spawn} from 'node:child_process';
import readline from 'node:readline';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
const dir=process.env.DUNLIN_WORKBENCH_REPORT;await fs.mkdir(dir,{recursive:true});
const child=spawn(path.resolve('build/v51/current-core/python-asan'),['tests/dsh_workbench_owner.py',path.resolve(process.env.DUNLIN_WORKBENCH_LIBRARY),path.resolve(process.env.DUNLIN_WORKBENCH_ASSETS),'reports/v51-current/real-dsh-published-inputs.json'],{stdio:['pipe','pipe','pipe'],env:{...process.env,DUNLIN_WORKBENCH_NATIVE_AGENTS:'1'}});
let stderr='';child.stderr.on('data',x=>stderr+=x);const reader=readline.createInterface({input:child.stdout});const init=await new Promise((resolve,reject)=>{reader.once('line',x=>resolve(JSON.parse(x)));child.once('exit',code=>reject(Error('owner exited '+code+' '+stderr)))});
const pause=ms=>new Promise(r=>setTimeout(r,ms));
async function call(route,data={}){const r=await(await fetch('http://127.0.0.1:'+init.port+route,{method:'POST',headers:{'X-Dunlin-Owner':init.token},body:JSON.stringify(data)})).json();if(!r.ok)throw Object.assign(Error(r.error),{failure:r.failure});return r.value}
const command=(command,args)=>call('/invoke',{plugin:'dsh-better-sidebar',command,args});
async function api(method,args){let r=await command('http',{url:'/sidebar/api/'+method,method:'POST',body:args});if(r.pendingHandle){const id=r.pendingHandle;try{for(let i=0;i<300;i++){const next=await command('http.pending',{id});if(next.ready){r=next.response;break}await pause(50)}}finally{await command('http.pending',{id,release:true})}}assert.equal(r.status,200,JSON.stringify(r));assert.equal(r.body.ok,true,JSON.stringify(r));return r.body.value}
const ws=(op,args={})=>command('ws:/sidebar/ws/sidechat',{op,_surface:'owned-push',...args});
const results=[];
try{
 const {childId}=await api('sidechat.start',{sessionId:'ui-chat'});
 const {id}=await ws('open',{url:'/sidebar/ws/sidechat'});
 await ws('send',{id,text:JSON.stringify({op:'watch',threadIds:[childId]})});await pause(150);await ws('read',{id});
 for(let n=0;n<2;n++){
  if(n)await api('sidechat.model',{childId,provider:'dunlin-native',model:'owned-b'});
  await api('sidechat.prompt',{childId,text:'Owned push round '+n+' · 中文'});
  // Drain notices caused by initial submission before the model can finish.
  await pause(200);await ws('read',{id});await pause(150);await ws('read',{id});
  const execution=await call('/native-agents-run');assert.equal(execution.runs.length,1,JSON.stringify(execution));assert.equal(execution.runs[0].run.status,'completed');
  let received=[];for(let i=0;i<25;i++){received.push(...(await ws('read',{id})).events);if(received.some(e=>e.type==='message'&&JSON.parse(e.text).threadId===childId))break;await pause(100)}
  assert.ok(received.some(e=>e.type==='message'&&JSON.parse(e.text).threadId===childId),'connected push never announced completed C reply '+n);
  const transcript=await api('sidechat.events',{childId});await fs.writeFile(path.join(dir,'transcript-'+n+'.json'),JSON.stringify(transcript,null,2));const dump=JSON.stringify(transcript);assert.match(dump,/Owned push round 0/);if(n)assert.match(dump,/Owned push round 1/);
  const answers=(dump.match(/Owned React baseline completed\./g)??[]).length;assert.equal(answers,n+1,'own transcript must contain exactly one answer per completed C job; inherited seed is excluded');
  results.push({round:n,notices:received.filter(e=>e.type==='message').map(e=>JSON.parse(e.text)),completedAnswers:answers,actualCExecution:true});
 }
 await ws('send',{id,text:JSON.stringify({op:'watch',threadIds:[]})});await pause(150);await ws('read',{id});
 await api('sidechat.prompt',{childId,text:'Owned hidden watch round'});await pause(150);await ws('read',{id});
 const hidden=await call('/native-agents-run');assert.equal(hidden.runs.length,1);assert.equal(hidden.runs[0].run.status,'completed');
 await pause(150);assert.deepEqual((await ws('read',{id})).events,[],'empty watch still refreshed a child');
 await ws('send',{id,text:JSON.stringify({op:'watch',threadIds:[childId]})});
 let resumed=[];for(let i=0;i<25;i++){resumed.push(...(await ws('read',{id})).events);if(resumed.some(e=>e.type==='message'&&JSON.parse(e.text).threadId===childId))break;await pause(100)}
 assert.ok(resumed.some(e=>e.type==='message'&&JSON.parse(e.text).threadId===childId),'watch replacement did not catch up C history');
 const alien=await ws('open',{url:'/sidebar/ws/sidechat'});await ws('send',{id:alien.id,text:JSON.stringify({op:'watch',threadIds:['not-an-owned-child']})});
 await assert.rejects(ws('read',{id:alien.id}),e=>e.failure?.data?.data?.code===-32602);await ws('close',{id:alien.id});
 assert.ok((await call('/manage',{op:'extensions.clients'})).some(row=>row.package==='dsh-better-sidebar'),'invalid watch quarantined owning plugin');
 results.push({watchReplacement:true,emptyWatchDoesNotRefresh:true,missingSourceRefused:true,invalidWatchDoesNotQuarantine:true});
 await ws('close',{id});await assert.rejects(ws('read',{id}),e=>e.failure?.data?.data?.code===-32602);
 assert.equal((await call('/native-agent-jobs')).modelRequests,4,'observing replayed a model');
 console.log('PASS actual connected upstream push announces both C-completed replies without transcript polling or replay');
}finally{
 child.stdin.end('\n');await new Promise(resolve=>child.once('exit',resolve));
 await fs.writeFile(path.join(dir,'results.json'),JSON.stringify(results,null,2));await fs.writeFile(path.join(dir,'owner-stderr.log'),stderr);
 await fs.writeFile(path.join(dir,'test-inputs.json'),JSON.stringify({library:process.env.DUNLIN_WORKBENCH_LIBRARY,assets:process.env.DUNLIN_WORKBENCH_ASSETS,testSHA256:createHash('sha256').update(await fs.readFile('tests/dsh_workbench_push.mjs')).digest('hex')},null,2));
}
