// Original Project handler + C admission/executor. Owned loopback fixture only.
import fs from 'node:fs/promises';
import path from 'node:path';
import crypto from 'node:crypto';
import {spawn} from 'node:child_process';
import readline from 'node:readline';
import assert from 'node:assert/strict';
const mode=process.env.DUNLIN_HELPER_CASE,dir=process.env.DUNLIN_WORKBENCH_REPORT;
assert.ok(['plan','submit','effect-receipt','handoff','result','lease','stale','disable','timeout','provider','long','empty','large','cleanup','unsupported-effort','foreign-session','unknown-model','context','utf16','trim','split-surrogate','engine-restart','engine-stop','engine-stop-receipt'].includes(mode));
await fs.mkdir(dir,{recursive:true});
await fs.writeFile(path.join(dir,'test-inputs.json'),JSON.stringify({mode,at:new Date().toISOString(),inputs:await Promise.all(['tests/dsh_model_helper_failures.mjs','tests/dsh_workbench_owner.py',process.env.DUNLIN_WORKBENCH_LIBRARY,path.join(process.env.DUNLIN_WORKBENCH_ASSETS,'assets.json')].map(async p=>({path:p,sha256:crypto.createHash('sha256').update(await fs.readFile(p)).digest('hex')})))},null,2));
const owner=spawn(path.resolve('build/v51/current-core/python-asan'),['tests/dsh_workbench_owner.py',path.resolve(process.env.DUNLIN_WORKBENCH_LIBRARY),path.resolve(process.env.DUNLIN_WORKBENCH_ASSETS),'reports/v51-current/real-dsh-published-inputs.json'],{stdio:['pipe','pipe','pipe'],env:{...process.env,DUNLIN_WORKBENCH_NATIVE_AGENTS:'1'}});
let stderr='';owner.stdin.on('error',()=>{});owner.stderr.on('data',s=>stderr+=s);
const lines=readline.createInterface({input:owner.stdout});
const init=await new Promise((r,j)=>{lines.once('line',s=>r(JSON.parse(s)));owner.once('exit',code=>j(Error('Owner startup '+code+' '+stderr)))});
async function call(route,body={}){const v=await(await fetch('http://127.0.0.1:'+init.port+route,{method:'POST',headers:{'X-Dunlin-Owner':init.token},body:JSON.stringify(body)})).json();if(!v.ok)throw Object.assign(Error(v.error),{failure:v});return v.value}
async function invoke(command,args){return call('/invoke',{plugin:'dsh-project-manager',command,args})}
const pause=ms=>new Promise(r=>setTimeout(r,ms));
let handle,failed,terminal,observedResponsive=false;
try{
 const before=await call('/native-helpers-state');
 if(mode==='context')await call('/native-helper-profile-change',{smallContext:true});
 if(['plan','submit','effect-receipt','handoff','result'].includes(mode))await call('/native-helper-audit',{deny:mode});
 if(['timeout','provider','long','empty','large'].includes(mode))await call('/native-text-fixture',{delay:mode==='timeout'?22:mode==='long'?18:0,fail:mode==='provider',empty:mode==='empty',large:mode==='large'});
 try{
  const first=await invoke('http',{method:'POST',url:'/_dsh/dsh-project-manager/api/goal/title',body:{session:'ui-chat',text:mode==='context'?'Owned context '+('x'.repeat(3000)):mode==='utf16'?' '+('🦆'.repeat(7000))+'tail \n':mode==='split-surrogate'?('x'.repeat(11999))+'🦆':mode==='trim'?'\uFEFF\u2000owned task\u3000\n':'Owned helper fault boundary '+mode,route:{provider:'dunlin-native',model:mode==='unknown-model'?'missing-profile':'owned-a',...(mode==='unsupported-effort'?{effort:'low'}:{})},...(mode==='foreign-session'?{session:'foreign-chat'}:{})},_surface:'owned-helper'});
  handle=first.pendingHandle;
  if(handle){
   if(mode==='stale')await call('/native-helper-profile-change');
   if(mode==='engine-restart')await call('/native-runtime-restart');
   if(mode==='engine-stop-receipt')await call('/native-helper-audit',{deny:'engine-stop'});
   if(['engine-stop','engine-stop-receipt'].includes(mode))await call('/native-runtime-disable');
   if(mode==='disable')await call('/manage',{op:'extensions.bind',plugin:'dsh-project-manager',arguments:{enabled:false}});
   if(mode==='lease')await invoke('http.pending',{id:handle,_surface:'owned-helper',release:true});
   if(['lease','stale','disable','engine-restart','engine-stop'].includes(mode)){
    const started=await call('/native-helpers-start');assert.equal(started.started.length,0,'revoked helper obtained execution');
   }else for(let i=0;i<450;i++){
    await call('/native-helpers-start');
    if(mode==='long'&&!observedResponsive){
     const state=await call('/native-helpers-state');
     if(state.jobs.some(j=>j.state==='running')){const other=await call('/invoke',{plugin:'dsh-better-sidebar',command:'http',args:{method:'POST',url:'/sidebar/api/settings.get',body:{}}});assert.equal(other.status,200);assert.equal(other.body.ok,true);observedResponsive=true;}
    }
    const next=await invoke('http.pending',{id:handle,_surface:'owned-helper'});
    if(next.ready){terminal=next.response;break}
    await pause(100);
   }
  }else terminal=first;
 }catch(error){failed={message:error.message,failure:error.failure}}
 if(handle){try{await invoke('http.pending',{id:handle,_surface:'owned-helper',release:true})}catch(error){failed??={message:error.message,failure:error.failure}}}
 // Wait for the actual C worker to settle cancellation, not just HTTP release.
 let after;for(let i=0;i<80;i++){after=await call('/native-helpers-state');if(!after.jobs.some(j=>['running','cancel_requested'].includes(j.state)))break;await pause(100)}
 await fs.writeFile(path.join(dir,'observed.json'),JSON.stringify({mode,before,after,terminal,failed,observedResponsive},null,2));
 assert.equal(after.parent,before.parent,'helper changed ordinary chat history');assert.deepEqual(after.counts,before.counts,'helper created chats/projects');
 const requests=after.requests.length-before.requests.length,quarantined=['effect-receipt','handoff','result'].includes(mode);
 assert.equal(after.inventory.find(p=>p.package==='dsh-project-manager').active,!quarantined&&!['disable','engine-stop','engine-stop-receipt'].includes(mode));
 if(mode==='engine-stop-receipt')assert.ok(after.inventory.every(p=>!p.active),'engine with failed stop receipt remained active');else if(mode==='engine-stop')assert.ok(after.inventory.every(p=>!p.active&&p.enabled),'explicit engine stop must retain desired bindings');else assert.ok(after.inventory.filter(p=>p.package!=='dsh-project-manager').every(p=>p.active),'unrelated plugin disabled');
 if(mode==='engine-stop-receipt'){assert.ok(failed);assert.equal(requests,0);assert.equal(after.inventory.find(p=>p.package==='dsh-project-manager').enabled,false);assert.ok(after.faults.some(f=>f.package==='dsh-project-manager'&&f.error==='environment_shutdown_receipt_unknown'));const scope=await call('/inspect');assert.ok(scope.requests.every(r=>r.state!=='running'),'failed engine stop receipt did not retain unknown outcome')}
 else if(quarantined){assert.ok(failed,'unknown receipt was reported as success');assert.ok(after.faults.some(f=>f.package==='dsh-project-manager'&&f.diagnostic?.includes('Partial effects are retained')));assert.equal(requests,mode==='result'?1:0);if(mode==='result')assert.ok(after.jobs.some(j=>j.state==='unknown'));}
 else if(['plan','submit','lease','stale','disable','unsupported-effort','foreign-session','unknown-model','context','split-surrogate','engine-restart','engine-stop'].includes(mode)){assert.equal(requests,0);assert.equal(after.faults.length,0);if(['plan','unsupported-effort','foreign-session','unknown-model','context','split-surrogate'].includes(mode))assert.ok(failed);if(mode==='submit')assert.equal(terminal?.body.ok,false);}
 else if(['long','cleanup','utf16','trim'].includes(mode)){if(mode==='long')assert.ok(observedResponsive);assert.equal(terminal?.body.ok,true,JSON.stringify({terminal,failed}));assert.equal(requests,1);assert.equal(after.jobs[0].state,'reported');}
 else if(mode==='empty'){assert.equal(terminal?.body.ok,false);assert.equal(terminal?.body.error.code,'EMPTY_TITLE');assert.equal(requests,2,'empty-title retry must be exactly once');assert.equal(after.jobs.length,2);}
 else{assert.equal(terminal?.body.ok,false,JSON.stringify({terminal,failed}));assert.equal(requests,1,'provider/timeout/oversized result must not retry');assert.ok(after.jobs.every(j=>['cancelled','failed'].includes(j.state)));}
 assert.ok(after.requests.slice(before.requests.length).every(r=>!r.tools&&!r.tool_choice));
 if(mode==='utf16')assert.equal(after.requests.at(-1).messages[1].content,'任务说明：\n'+('🦆'.repeat(6000)));
 if(mode==='trim')assert.equal(after.requests.at(-1).messages[1].content,'任务说明：\nowned task');
 if(['engine-restart','engine-stop'].includes(mode)){assert.ok(after.jobs.every(j=>j.state==='cancelled'),'clean engine restart retained unclaimed helper authority');const scope=await call('/inspect');assert.ok(scope.requests.every(r=>r.state!=='running'),'clean engine restart retained a running HTTP lease')}
 if(mode==='cleanup'){await call('/native-helper-audit',{deny:'cleanup'});assert.equal(await call('/delete-fixture-chat'),false,'cleanup failure deleted source conversation');const retained=await call('/native-helpers-state');assert.equal(retained.parent,before.parent);assert.equal(retained.jobs.length,1);await call('/native-helper-audit',{});assert.equal(await call('/delete-fixture-chat'),true);const cleared=await call('/native-helpers-state');await fs.writeFile(path.join(dir,'cleanup.json'),JSON.stringify(cleared,null,2));assert.equal(cleared.jobs.length,0,'deleted chat retained helper task/result cache');assert.equal(cleared.plans.length,0,'deleted chat retained helper task input');assert.equal(cleared.counts.chats,before.counts.chats-1)}
 console.log('PASS',mode,'actual original handler/C execution and negative boundary');
}finally{
 await fs.writeFile(path.join(dir,'owner-stderr.log'),stderr);owner.stdin.end('\n');const code=(owner.exitCode!==null||owner.signalCode!==null)?owner.exitCode:await new Promise(r=>owner.once('exit',r));await fs.writeFile(path.join(dir,'owner-stderr.log'),stderr);assert.equal(code,0,stderr);
}
