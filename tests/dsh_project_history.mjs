// Original published Project history endpoint, real C source/SDK host.
import fs from 'node:fs/promises';import path from 'node:path';import {spawn} from 'node:child_process';import readline from 'node:readline';import assert from 'node:assert/strict';import {createHash} from 'node:crypto';
const dir=process.env.DUNLIN_WORKBENCH_REPORT;await fs.mkdir(dir,{recursive:true});const library=path.resolve(process.env.DUNLIN_WORKBENCH_LIBRARY),assets=path.resolve(process.env.DUNLIN_WORKBENCH_ASSETS);
await fs.writeFile(path.join(dir,'test-inputs.json'),JSON.stringify({at:new Date().toISOString(),library,assets,inputs:await Promise.all([library,path.join(path.dirname(library),'inputs.json'),new URL(import.meta.url).pathname,'tests/dsh_workbench_owner.py'].map(async file=>({path:file,sha256:createHash('sha256').update(await fs.readFile(file)).digest('hex')})))},null,2));
const child=spawn(path.resolve('build/v51/current-core/python-asan'),['tests/dsh_workbench_owner.py',library,assets,'reports/v51-current/real-dsh-published-inputs.json'],{stdio:['pipe','pipe','pipe'],env:{...process.env,DUNLIN_WORKBENCH_NATIVE_AGENTS:'1',DUNLIN_WORKBENCH_HISTORY_FIXTURE:'1'}});let stderr='';child.stderr.on('data',x=>stderr+=x);const reader=readline.createInterface({input:child.stdout});const init=await new Promise((resolve,reject)=>{reader.once('line',x=>resolve(JSON.parse(x)));child.once('exit',code=>reject(Error('owner exited '+code+' '+stderr)))});
async function call(route,data={}){const r=await(await fetch('http://127.0.0.1:'+init.port+route,{method:'POST',headers:{'X-Dunlin-Owner':init.token},body:JSON.stringify(data)})).json();assert.ok(r.ok,JSON.stringify(r));return r.value}
async function history(id,extra=''){return call('/invoke',{plugin:'dsh-project-manager',command:'http',args:{url:'/_dsh/dsh-project-manager/api/session/messages?id='+encodeURIComponent(id)+extra,method:'GET'}})}
const evidence={checks:[],limits:'Original Project HTTP and C canonical source; browser/native picker separate. Unknown source timestamps must remain blank rather than fabricated activity.'};
try{
 const before=await call('/native-state-fixture',{chat:'ui-chat'});const result=await history('ui-chat');evidence.initial=result;
 assert.equal(result.status,200,JSON.stringify(result));assert.equal(result.body.ok,true,JSON.stringify(result));assert.equal(result.body.messages.length,1);assert.equal(result.body.messages[0].text,'Owned baseline for React checkpoint and fork');assert.equal(result.body.messages[0].at,'','unknown C timestamp fabricated a date');
 const row=result.body.messages[0];const empty=await history('ui-chat','&before='+row.seq+'&limit=1');assert.equal(empty.status,200);assert.deepEqual(empty.body.messages,[]);assert.equal(empty.body.total,1);
 const denied=await history('foreign-chat');assert.ok(denied.status>=400);evidence.denied=denied;assert.deepEqual(await call('/native-state-fixture',{chat:'ui-chat'}),before,'history read changed canonical source');
 await call('/restart');await call('/manage',{op:'extensions.assets',arguments:{path:assets}});await call('/native-runtime-restart');const cold=await history('ui-chat');assert.equal(cold.status,200);assert.deepEqual(cold.body.messages,result.body.messages);assert.equal((await call('/native-agents-results')).requests,1);
 evidence.checks.push({case:'original-history-pagination-unknown-time-and-cold',status:'pass'});
 async function post(name,body){const r=await call('/invoke',{plugin:'dsh-project-manager',command:'http',args:{url:'/_dsh/dsh-project-manager/api/'+name,method:'POST',body}});assert.equal(r.status,200,JSON.stringify(r));assert.equal(r.body.ok,true,JSON.stringify(r));return r.body}
 const group=(await post('group/create',{name:'Owned cold history group'})).result;
 const card=(await post('card/create',{group:group.id,title:'Owned cold history card'})).result;
 const prepared=await post('session/prepare',{cardId:card.id});
 const created=await call('/invoke',{plugin:'dsh-project-manager',command:'native.session.create',args:{workspaceId:prepared.workspaceId}});const id=created.value.id;
 await post('session/bind',{sessionId:id,cardId:card.id,title:card.t});
 assert.equal((await call('/model',{chat:id,input:'Owned cold Project history human marker'})).run.status,'completed');
 const snap=()=>call('/invoke',{plugin:'dsh-project-manager',command:'native.snapshot',args:{}});
 let snapshot=await snap();assert.ok(snapshot.workspaces.items.find(row=>row.workspaceId===prepared.workspaceId)?.sessionIds.includes(id));
 await call('/restart');await call('/manage',{op:'extensions.assets',arguments:{path:assets}});await call('/native-runtime-restart');
 snapshot=await snap();evidence.coldGroup={group:group.id,workspace:prepared.workspaceId,session:id,snapshot};
 await fs.writeFile(path.join(dir,'cold-project.json'),JSON.stringify(evidence.coldGroup,null,2));
 assert.ok(snapshot.workspaces.items.find(row=>row.workspaceId===prepared.workspaceId)?.sessionIds.includes(id),'cold Project workspace lost actual C session membership');
 const projectHistory=await history(id);assert.equal(projectHistory.status,200,JSON.stringify(projectHistory));assert.deepEqual(projectHistory.body.messages.map(row=>row.text),['Owned cold Project history human marker']);assert.equal((await call('/native-agents-results')).requests,2,'cold Project query replayed model');
 evidence.checks.push({case:'cold-project-membership-and-human-context',status:'pass'});
 const swapped=await call('/native-workspace-swap',{chat:id});assert.equal(swapped.available,false);snapshot=await snap();assert.ok(!snapshot.workspaces.items.find(row=>row.workspaceId===prepared.workspaceId)?.sessionIds.includes(id),'replaced directory inherited original workspace authority');
 const restoredPath=await call('/native-workspace-swap',{chat:id,restore:true});assert.equal(restoredPath.available,true);snapshot=await snap();assert.ok(snapshot.workspaces.items.find(row=>row.workspaceId===prepared.workspaceId)?.sessionIds.includes(id));
 evidence.checks.push({case:'changed-inode-refuses-membership-and-original-directory-restores',status:'pass'});
 async function calendar(wantDay,wantUnknown){
  let body;for(let attempt=0;attempt<40;attempt++){
   const r=await call('/invoke',{plugin:'dsh-project-manager',command:'http',args:{url:'/_dsh/dsh-project-manager/api/gantt?warm=1',method:'GET'}});assert.equal(r.status,200,JSON.stringify(r));body=r.body;assert.equal(body.ok,true,JSON.stringify(body));
   const days=body.cards.find(row=>row.id===card.id).auto;assert.ok(!days.some(day=>day.startsWith('1970')),'unknown history fabricated 1970 activity');
   if((wantDay?days.includes(body.today):days.length===0)&&(!wantUnknown||body.degraded.some(text=>text.includes('部分历史没有逐事件时间'))))return body;
   await new Promise(resolve=>setTimeout(resolve,100));
  }assert.fail('original calendar did not converge to actual source times: '+JSON.stringify(body));
 }
 evidence.knownCalendar=await calendar(true,false);
 await call('/native-legacy-event-time',{chat:id});await call('/restart');await call('/manage',{op:'extensions.assets',arguments:{path:assets}});await call('/native-runtime-restart');
 evidence.legacyCalendar=await calendar(false,true);
 assert.equal((await call('/model',{chat:id,input:'Owned known-time continuation after legacy source'})).run.status,'completed');
 await call('/native-runtime-restart');evidence.mixedCalendar=await calendar(true,true);
 assert.equal((await call('/native-agents-results')).requests,3,'activity query started a model');evidence.checks.push({case:'original-calendar-known-legacy-and-mixed-source-time',status:'pass'});
 console.log('PASS original Project human history source/pagination/unknown-time/cold project membership/refusal; explicit owned C turns only');
}finally{child.stdin.end('\n');await new Promise(resolve=>child.exitCode!==null?resolve():child.once('exit',resolve));await fs.writeFile(path.join(dir,'result.json'),JSON.stringify(evidence,null,2));await fs.writeFile(path.join(dir,'owner-stderr.log'),stderr)}
