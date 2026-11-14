// Kill ONLY this test's spawned C owner while its actual helper HTTP is running.
// The independent local model fixture stays alive to detect any recovery replay.
import fs from 'node:fs/promises';import path from 'node:path';import os from 'node:os';
import crypto from 'node:crypto';import assert from 'node:assert/strict';
import {spawn,execFileSync} from 'node:child_process';import readline from 'node:readline';
const dir=process.env.DUNLIN_WORKBENCH_REPORT,library=path.resolve(process.env.DUNLIN_WORKBENCH_LIBRARY),assets=path.resolve(process.env.DUNLIN_WORKBENCH_ASSETS);
await fs.mkdir(dir,{recursive:true});
const inputs=['tests/dsh_model_helper_crash.mjs','tests/fixtures/recover_model_helper.py','tests/fixtures/native_text_image_server.py','tests/dsh_workbench_owner.py',library,path.join(assets,'assets.json')];
await fs.writeFile(path.join(dir,'test-inputs.json'),JSON.stringify({at:new Date().toISOString(),inputs:await Promise.all(inputs.map(async p=>({path:p,sha256:crypto.createHash('sha256').update(await fs.readFile(p)).digest('hex')})))},null,2));
const python=path.resolve('build/v51/current-core/python-asan'),pause=ms=>new Promise(r=>setTimeout(r,ms));
let service,owner,home,nonce,stderr='',modelStderr='',killed=false;
function firstLine(child){return new Promise((resolve,reject)=>{const lines=readline.createInterface({input:child.stdout});lines.once('line',value=>resolve(value));child.once('exit',(code,signal)=>reject(Error('Startup exit '+code+' '+signal)));})}
function exited(child){return child.exitCode!==null||child.signalCode!==null?Promise.resolve({code:child.exitCode,signal:child.signalCode}):new Promise(resolve=>child.once('exit',(code,signal)=>resolve({code,signal})));}
try{
 service=spawn('python3',['-B','tests/fixtures/native_text_image_server.py','--port','0','--text-delay','18','--report',path.join(dir,'model')],{stdio:['pipe','pipe','pipe']});service.stdin.on('error',()=>{});service.stderr.on('data',s=>modelStderr+=s);const line=await firstLine(service),endpoint=line.match(/http:\/\/127\.0\.0\.1:\d+\/v1/)?.[0];assert.ok(endpoint,line);
 owner=spawn(python,['tests/dsh_workbench_owner.py',library,assets,'reports/v51-current/real-dsh-published-inputs.json'],{stdio:['pipe','pipe','pipe'],env:{...process.env,DUNLIN_WORKBENCH_NATIVE_AGENTS:'1',DUNLIN_WORKBENCH_MODEL_URL:endpoint}});owner.stdin.on('error',()=>{});owner.stderr.on('data',s=>stderr+=s);const init=JSON.parse(await firstLine(owner));
 home=await fs.realpath(path.dirname(init.workspace));const temp=await fs.realpath(os.tmpdir());assert.ok(home.startsWith(temp+path.sep)&&path.basename(home).startsWith('dunlin-react-owner-'));assert.equal((await fs.stat(home)).uid,process.getuid());nonce=crypto.randomUUID();await fs.writeFile(path.join(home,'helper-crash-owned.json'),nonce,{mode:0o600});
 async function call(route,body={}){const r=await(await fetch('http://127.0.0.1:'+init.port+route,{method:'POST',headers:{'X-Dunlin-Owner':init.token},body:JSON.stringify(body)})).json();assert.ok(r.ok,JSON.stringify(r));return r.value}
 const before=await call('/native-helpers-state');
 const submitted=await call('/invoke',{plugin:'dsh-project-manager',command:'http',args:{method:'POST',url:'/_dsh/dsh-project-manager/api/goal/title',body:{session:'ui-chat',text:'Owned helper crash must never cause replay',route:{provider:'dunlin-native',model:'owned-a'}}}});assert.ok(submitted.pendingHandle);
 await call('/native-helpers-start');let running;
 for(let i=0;i<80;i++){running=await call('/native-helpers-state');let requests=[];try{requests=(await fs.readFile(path.join(dir,'model/requests.jsonl'),'utf8')).trim().split('\n')}catch{}if(running.jobs[0]?.state==='running'&&requests.length===1)break;await pause(50)}
 assert.equal(running.jobs[0]?.state,'running');await fs.writeFile(path.join(dir,'before-crash.json'),JSON.stringify({before,running},null,2));
 // Metadata only: track the exact worker originally parented by this owned PID.
 const children=execFileSync('ps',['-axo','pid=,ppid=,command='],{encoding:'utf8'}).split('\n').map(s=>s.trim().split(/\s+/,3)).filter(a=>Number(a[1])===owner.pid&&a[2]?.includes('node'));
 assert.equal(owner.exitCode,null);assert.ok(owner.kill('SIGKILL'));const death=await exited(owner);assert.equal(death.signal,'SIGKILL');killed=true;
 // EOF on the private pipes disposes the optional host; never signal an
 // unrelated process. Check only initially owned PIDs and require they exit.
 for(let i=0;i<60;i++){const pids=new Set(execFileSync('ps',['-axo','pid='],{encoding:'utf8'}).trim().split(/\s+/).map(Number));if(children.every(([pid])=>!pids.has(Number(pid))))break;await pause(50)}
 const live=new Set(execFileSync('ps',['-axo','pid='],{encoding:'utf8'}).trim().split(/\s+/).map(Number));assert.ok(children.every(([pid])=>!live.has(Number(pid))),'owned compatibility child did not dispose after parent EOF');
 const recovery=spawn(python,['tests/fixtures/recover_model_helper.py',library,home,nonce],{stdio:['ignore','pipe','pipe']});let output='',errors='';recovery.stdout.on('data',s=>output+=s);recovery.stderr.on('data',s=>errors+=s);const result=await exited(recovery);await fs.writeFile(path.join(dir,'recovery-stderr.log'),errors);assert.equal(result.code,0,errors);const recovered=JSON.parse(output);assert.equal(recovered.parentSHA,crypto.createHash('sha256').update(before.parent).digest('hex'));
 const requests=(await fs.readFile(path.join(dir,'model/requests.jsonl'),'utf8')).trim().split('\n').map(JSON.parse);assert.equal(requests.length,1,'recovery repeated actual provider request');await fs.writeFile(path.join(dir,'result.json'),JSON.stringify({actualOwnerSIGKILL:true,actualProviderRequests:1,recovered},null,2));console.log('PASS actual running C helper owner crash -> unknown/disabled/no HTTP replay, source preserved');
}finally{
 if(owner&&!killed){owner.stdin.end('\n');await exited(owner)}
 if(service){service.stdin.end('\n');await exited(service)}
 await fs.writeFile(path.join(dir,'owner-stderr.log'),stderr);await fs.writeFile(path.join(dir,'model-stderr.log'),modelStderr);
 if(home&&nonce&&await fs.readFile(path.join(home,'helper-crash-owned.json'),'utf8')===nonce)await fs.rm(home,{recursive:true});
}
