import assert from 'node:assert/strict';
export async function checkpointActions({w,request,button,pause,fixture,clipboard}){
 const timeline=async()=>{const value=await request('rpc',{namespace:'checkpointPanel',method:'timeline',args:[200]});assert.equal(value.ok,true);return value.value};
 const initial=await timeline();
 await fixture({write:'checkpoint-changed.txt',text:'Owned manual checkpoint baseline\n'});
 const one=await request('slash:checkpoint',{input:'note React before'});assert.match(one.result.text,/^checkpoint: captured #/);const baseline=await timeline();const unchanged=await request('slash:checkpoint',{input:'note must deduplicate'});assert.match(unchanged.result.text,/^checkpoint: workspace unchanged/);assert.equal((await timeline()).total,baseline.total,'unchanged capture created a fake snapshot');
 await fixture({write:'checkpoint-changed.txt',text:'Actual changed workspace bytes\n'});
 const two=await request('slash:checkpoint',{input:'note React after'});assert.match(two.result.text,/^checkpoint: captured #/);
 const after=await timeline();assert.equal(after.total,initial.total+2,'manual captures did not add exactly two records');for(const row of initial.rows)assert.ok(after.rows.some(r=>r.id===row.id),'existing automatic checkpoint was removed');
 const before=after.rows.filter(row=>row.note==='React before'),later=after.rows.filter(row=>row.note==='React after');assert.equal(before.length,1);assert.equal(later.length,1);assert.equal(before[0].sessionId,'ui-chat');assert.equal(later[0].sessionId,'ui-chat');
 button('刷新').click();for(let i=0;i<30&&w.document.querySelectorAll('.dcr-row').length<after.total;i++)await pause(100);assert.equal(w.document.querySelectorAll('.dcr-row').length,after.total,'real timeline rows missing');
 const rows=[...w.document.querySelectorAll('.dcr-row')];const find=record=>rows.find(row=>row.querySelector('.dcr-id')?.textContent==='#'+record.id.slice(0,8));const select=(row,label)=>[...row.querySelectorAll('button')].find(b=>b.textContent===label);assert.ok(find(before[0])&&find(later[0]));select(find(before[0]),'选为 A（较早）').click();select(find(later[0]),'选为 B（较新）').click();for(let i=0;i<30&&!w.document.body.textContent.includes('checkpoint-changed.txt');i++)await pause(100);assert.match(w.document.body.textContent,/checkpoint-changed.txt/,'original diff did not show actual new file');assert.ok(!w.document.querySelector('.dcr-error'),'checkpoint UI reports error');
 assert.match(w.document.body.textContent,/A 备注: React before/,'older A note label is reversed');assert.match(w.document.body.textContent,/B 备注: React after/,'newer B note label is reversed');
 if(clipboard){const copy=button('复制 /rewind');assert.ok(copy,'original copy command button missing');copy.click();for(let i=0;i<30&&!w.document.body.textContent.includes('已复制');i++)await pause(50);assert.ok(w.document.body.textContent.includes('已复制'));const writes=await clipboard();assert.equal(writes.at(-1),'/rewind '+later[0].id.slice(0,8),'original copy button did not reach native clipboard port')}
 console.log('CHECKPOINT CHECKS real C position; two original snapshots; unchanged React timeline and actual file diff');
}
