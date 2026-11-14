// Projection contracts only; actual C execution/SDK/writer tests are separate.
import assert from 'node:assert/strict';
import {nativeHistoryEvents,nativeRestoreOptions} from '../hosts/node-dsh/native_sessions.mjs';
const messages=[{role:'user',content:'parent'},{role:'assistant',content:'parent answer'},{role:'user',content:'child failure'}];
const detail='a'.repeat(2047)+'🦆末尾';
const source={cwd:'/owned',state:{version:1,messages,active:false,status:1,detail}};
const before=JSON.stringify(source),legacy=nativeHistoryEvents('owned',source);
const persisted=structuredClone(source);persisted.state._dunlin_turn_outcomes=[{message:2,through:2,status:1,detail:'a'.repeat(2047),truncated:true,run:'owned-run'}];
assert.deepEqual(legacy,nativeHistoryEvents('owned',persisted),'legacy failure truncation differs from subsequent actual C metadata');
const after=structuredClone(persisted);after.state.messages.push({role:'user',content:'explicit next input'},{role:'assistant',content:'next answer'});after.state.status=0;
assert.deepEqual(nativeHistoryEvents('owned',after).slice(0,legacy.length),legacy,'later success rewrites prior actual failure');
const child={...persisted,agent:{parentSession:'parent',origin:'subagent'},inheritedThrough:1};const restored=nativeRestoreOptions('owned',child);assert.equal(restored.seed.filter(e=>e.type==='turn/end'&&e.data.reason.kind==='error').length,1,'child failure incorrectly applied to inherited parent');
for(const status of [1,2,3,4,5,7]){const value={...source,state:{...source.state,status,detail:'actual terminal '+status}};const reason=nativeHistoryEvents('owned',value).at(-1).data.reason;assert.equal(reason.kind,status===2?'aborted':'error')}
assert.equal(JSON.stringify(source),before,'projection mutated canonical messages');
console.log('PASS native outcome projection preserves legacy UTF8 truncation, failed prefix, inherited boundary and non-success reasons; no canonical mutation');
