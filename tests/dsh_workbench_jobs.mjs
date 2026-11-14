import assert from 'node:assert/strict';
export async function jobsActions({w,request,pause,button,run}){
 const task='Owned original Tasks native job';const result=await request('native.session.prompt',{id:'ui-chat',mode:'queue',content:[{type:'text',text:task}]});assert.equal(result.value.accepted,true);
 const tab=button('任务管理');assert.ok(tab&&!tab.disabled,'original Tasks tab absent');tab.click();
 for(let i=0;i<40&&!w.document.body.textContent.includes(task);i++)await pause(100);
 assert.ok(w.document.body.textContent.includes(task),'original Tasks page does not show actual queued C job');
 console.log('ORIGINAL JOBS QUEUED',w.document.body.textContent.slice(-2400));
 const executed=await run();assert.equal(executed.runs.length,1);assert.equal(executed.runs[0].run.status,'completed');
 for(let i=0;i<40&&!w.document.querySelector('button[aria-label^="'+task+' 已完成"]');i++)await pause(100);assert.ok(w.document.querySelector('button[aria-label^="'+task+' 已完成"]'),'original job did not settle');
 console.log('ORIGINAL JOBS COMPLETED',w.document.body.textContent.slice(-2400));
 const row=[...w.document.querySelectorAll('button,[role=button]')].find(e=>e.textContent.includes(task));assert.ok(row,'original job row absent');row.click();
 for(let i=0;i<40&&!w.document.body.textContent.includes('Owned React baseline completed.');i++)await pause(100);
 assert.ok(w.document.body.textContent.includes('Owned React baseline completed.'),'original task observation does not show actual C output');
 const cancelTask='Owned original Tasks queued cancel';await request('native.session.prompt',{id:'ui-chat',mode:'queue',content:[{type:'text',text:cancelTask}]});
 for(let i=0;i<40&&!w.document.querySelector('button[aria-label="终止"]');i++)await pause(100);const stop=w.document.querySelector('button[aria-label="终止"]');assert.ok(stop,'original job cancellation control absent');stop.click();await pause(60);const confirm=w.document.querySelector('button[aria-label="再次点击确认终止"]');assert.ok(confirm,'original job kill confirmation absent');confirm.click();
 for(let i=0;i<40&&!w.document.querySelector('button[aria-label^="'+cancelTask+' 已终止"]');i++)await pause(100);assert.ok(w.document.querySelector('button[aria-label^="'+cancelTask+' 已终止"]'),'original cancel did not update actual C job state');assert.equal((await run()).runs.length,0,'UI-cancelled task still executed');
 console.log('JOBS CHECK actual C queued/completed job and original output observation, no consuming cursor or model replay');
}
