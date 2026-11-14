// Actual published Sidebar route/SDK -> real C worker -> paused loopback SSE.
// No browser or external provider claim; native rendering is checked separately.
import fs from 'node:fs/promises';
import path from 'node:path';
import {spawn} from 'node:child_process';
import readline from 'node:readline';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
const dir=process.env.DUNLIN_WORKBENCH_REPORT;await fs.mkdir(dir,{recursive:true});
const library=path.resolve(process.env.DUNLIN_WORKBENCH_LIBRARY),assets=path.resolve(process.env.DUNLIN_WORKBENCH_ASSETS);
const files=[library,path.join(path.dirname(library),'inputs.json'),import.meta.filename??new URL(import.meta.url).pathname,'tests/dsh_workbench_owner.py',path.join(assets,'hosts/node-dsh/services.mjs')];
await fs.writeFile(path.join(dir,'test-inputs.json'),JSON.stringify({at:new Date().toISOString(),command:process.argv,library,assets,inputs:await Promise.all(files.map(async file=>({path:file,sha256:createHash('sha256').update(await fs.readFile(file)).digest('hex')})))},null,2));
const child=spawn(path.resolve('build/v51/current-core/python-asan'),['tests/dsh_workbench_owner.py',library,assets,'reports/v51-current/real-dsh-published-inputs.json'],{stdio:['pipe','pipe','pipe'],env:{...process.env,DUNLIN_WORKBENCH_NATIVE_AGENTS:'1',DUNLIN_WORKBENCH_LIVE_STREAM:'1'}});
let stderr='';child.stderr.on('data',x=>stderr+=x);const reader=readline.createInterface({input:child.stdout});
const init=await new Promise((resolve,reject)=>{reader.once('line',x=>resolve(JSON.parse(x)));child.once('exit',code=>reject(Error('owner exited '+code+' '+stderr)))});
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));const evidence={checks:[],limits:'Actual C/SDK and original Sidebar HTTP, deterministic local SSE; native rendering/external providers separate.'};
async function call(route,data={}){const r=await(await fetch('http://127.0.0.1:'+init.port+route,{method:'POST',headers:{'X-Dunlin-Owner':init.token},body:JSON.stringify(data)})).json();assert.ok(r.ok,JSON.stringify(r));return r.value}
async function api(method,body){let r=await call('/invoke',{plugin:'dsh-better-sidebar',command:'http',args:{url:'/sidebar/api/'+method,method:'POST',body}});if(r.pendingHandle){const id=r.pendingHandle;try{for(let i=0;i<150;i++){const poll=await call('/invoke',{plugin:'dsh-better-sidebar',command:'http.pending',args:{id}});if(poll.ready){r=poll.response;break}await pause(50)}}finally{await call('/invoke',{plugin:'dsh-better-sidebar',command:'http.pending',args:{id,release:true}})}}assert.equal(r.status,200,JSON.stringify(r));assert.equal(r.body.ok,true,JSON.stringify(r));return r.body.value}
try{
 const {childId}=await api('sidechat.start',{sessionId:'ui-chat'});evidence.childId=childId;
 await api('sidechat.prompt',{childId,text:'Owned live-stream verification'});
 assert.equal((await call('/native-agents-start')).started.length,1);
 let live;for(let i=0;i<40;i++){live=await api('sidechat.events',{childId});if(live.live.length)break;await pause(50)}
 evidence.inFlight=live;await fs.writeFile(path.join(dir,'in-flight.json'),JSON.stringify(live,null,2));
 assert.ok(live.live.length,'original Sidebar never received in-flight C text');
 assert.equal(live.live.map(row=>row.data.chunk.text??'').join(''),'Owned live prefix · 中文🦆');
 const source=await call('/native-state-fixture',{chat:childId});assert.equal(source.active,true);assert.ok(!JSON.stringify(source.messages).includes('Owned live prefix'),'partial text entered canonical history');
 assert.ok((await call('/native-agents-results')).running>0,'provider was already settled');
 evidence.checks.push({case:'actual-C-text-visible-before-commit',status:'pass'});
 for(let i=0;i<120&&(await call('/native-agents-results')).running;i++)await pause(50);
 const settled=await api('sidechat.events',{childId});evidence.settled=settled;
 assert.deepEqual(settled.live,[]);const messages=settled.events.filter(row=>row.type==='assistant/message');
 assert.equal(messages.filter(row=>row.data.message.content.some(block=>block.text==='Owned live prefix · 中文🦆 settled once.')).length,1,'settled reply absent/duplicated');
 const count=(await call('/native-agents-results')).requests;assert.equal(count,2,'observation replayed a model');
 await call('/restart');await call('/manage',{op:'extensions.assets',arguments:{path:assets}});const restored=await call('/native-runtime-restart');assert.equal(restored.failed,0);const cold=await api('sidechat.events',{childId});assert.deepEqual(cold.live,[]);assert.equal((await call('/native-agents-results')).requests,count);
 evidence.checks.push({case:'settlement-and-cold-no-partial-or-replay',status:'pass'});
 // A second explicit turn must have a different attempt identity and an
 // audited original Cancel must clear preview without committing a fake answer.
 await api('sidechat.prompt',{childId,text:'Owned cancelled live-stream verification'});
 assert.equal((await call('/native-agents-start')).started.length,1);
 let second;for(let i=0;i<40;i++){second=await api('sidechat.events',{childId});if(second.live.length)break;await pause(50)}
 assert.ok(second.live.length,'second live attempt absent');assert.notEqual(second.live[0].data.attemptId,live.live[0].data.attemptId);
 await api('sidechat.cancel',{childId});
 for(let i=0;i<120&&(await call('/native-agents-results')).running;i++)await pause(50);
 const cancelled=await api('sidechat.events',{childId});assert.deepEqual(cancelled.live,[]);
 assert.ok(!cancelled.events.some(row=>row.type==='assistant/message'&&row.data.turn===second.live[0].data.turn),'cancelled partial became a completed assistant message');
 assert.equal((await call('/native-agents-results')).requests,3);evidence.cancelled=cancelled;
 evidence.checks.push({case:'new-attempt-and-cancel-no-false-commit',status:'pass'});
 console.log('PASS original Sidebar receives actual C partial text and clears it at settlement/cold restart');
}finally{
 child.stdin.end('\n');await new Promise(resolve=>child.exitCode!==null?resolve():child.once('exit',resolve));await fs.writeFile(path.join(dir,'result.json'),JSON.stringify(evidence,null,2));await fs.writeFile(path.join(dir,'owner-stderr.log'),stderr);
}
