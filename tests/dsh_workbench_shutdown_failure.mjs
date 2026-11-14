// Real five-package owner, real C admission and Node transports. All negatives
// must fail without poisoning another page/package or mutating test workspace.
import fs from 'node:fs/promises';import path from 'node:path';import {spawn} from 'node:child_process';import readline from 'node:readline';import {createHash} from 'node:crypto';import assert from 'node:assert/strict';
const child=spawn(path.resolve('build/v51/current-core/python-asan'),['tests/dsh_workbench_owner.py',path.resolve(process.env.DUNLIN_WORKBENCH_LIBRARY),path.resolve(process.env.DUNLIN_WORKBENCH_ASSETS),'reports/v51-current/real-dsh-published-inputs.json'],{stdio:['pipe','pipe','pipe']});let stderr='';child.stderr.on('data',x=>stderr+=x);const reader=readline.createInterface({input:child.stdout});const init=await new Promise((resolve,reject)=>{reader.once('line',x=>resolve(JSON.parse(x)));child.once('exit',code=>reject(Error('owner exited '+code+' '+stderr)))});
const dir=process.env.DUNLIN_WORKBENCH_REPORT;await fs.mkdir(dir,{recursive:true});
async function call(route,data){const r=await(await fetch('http://127.0.0.1:'+init.port+route,{method:'POST',headers:{'X-Dunlin-Owner':init.token},body:JSON.stringify(data)})).json();if(!r.ok)throw Object.assign(Error(r.error),{failure:r.failure});return r.value}
const command=(plugin,command,args)=>call('/invoke',{plugin,command,args});const image=(method,args)=>command('dsh-image-gen',method,args);
try{
 await call('/quiesce-audit',{deny:true});await assert.rejects(call('/manage',{op:'extensions.quiesce'}),e=>e.failure?.error==='extension_shutdown_not_confirmed');
 let observed=await call('/inspect',{});assert.equal(observed.shutdownCalls.length,1);assert.equal(observed.shutdownCalls[0].status,'running');const failed=observed.shutdownCalls[0].package;
 await call('/quiesce-audit',{deny:false});await call('/restart',{});await call('/manage',{op:'extensions.assets',arguments:{path:path.resolve(process.env.DUNLIN_WORKBENCH_ASSETS)}});await call('/manage',{op:'extensions.restore'});
 const plugins=await call('/manage',{op:'extensions.list'});assert.equal(plugins.find(row=>row.package===failed).enabled,false);assert.ok(plugins.filter(row=>row.package!==failed).every(row=>row.enabled));assert.equal((await call('/manage',{op:'extensions.clients'})).length,4);
 observed=await call('/inspect',{});assert.equal(observed.shutdownCalls.length,1);assert.equal(observed.shutdownCalls[0].status,'unknown','unconfirmed shutdown was falsely finalized/replayed');
 await fs.writeFile(path.join(dir,'result.json'),JSON.stringify({failedReceiptStopsAcknowledgment:true,unknownRetained:true,noReplay:true,onlyAffectedPluginQuarantined:true},null,2));console.log('PASS failed shutdown receipt stays unknown and cannot be acknowledged/replayed; unrelated bindings restore');
}finally{child.stdin.end('\n');await new Promise(resolve=>child.once('exit',resolve));await fs.writeFile(path.join(dir,'owner-stderr.log'),stderr)}
