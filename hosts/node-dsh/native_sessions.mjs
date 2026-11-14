// A derived SDK history view; C remains the canonical journal and executor.
import crypto from 'node:crypto';
const failure=message=>Object.assign(Error(message),{code:-32602});
export function readNativeSource(native,chat){
 let offset=0,revision,meta,parts=[];
 for(let page=0;page<48;page++){
  const result=native.call('session.source',{chat,offset,...(revision?{revision}:{})});
  if(!result?.ok)throw failure(result?.error??'Native source unavailable');
  const row=result.data;if(typeof row?.json!=='string'||!Number.isSafeInteger(row.next)||row.next<offset||!Number.isSafeInteger(row.total)||row.total>8388608||row.next>row.total||Buffer.byteLength(row.json)!==row.next-offset||!/^[a-f0-9]{64}$/.test(row.revision)||(revision&&revision!==row.revision))throw failure('Invalid native source page');
  revision=row.revision;meta??=row;parts.push(row.json);offset=row.next;
  if(offset===row.total){const json=parts.join('');if(crypto.createHash('sha256').update(json).digest('hex')!==revision)throw failure('Native source integrity mismatch');const state=JSON.parse(json);if(![1,2].includes(state.version)||!Array.isArray(state.messages)||typeof state.active!=='boolean')throw failure('Invalid canonical history');return {...meta,revision,state}}
  if(!row.json)throw failure('Native source made no progress');
 }
 throw failure('Native source exceeds page budget');
}
// Stable identities refer to the actual C sequence and bytes, not a guessed
// answer or a synthetic model execution. Unknown timestamps remain unknown(0).
export function nativeHistoryEvents(chat,source,boundaries){
 const events=[];let turn=0,step=0,open=false,userIndex=-1,lastIndex=-1,at=0;
 const outcomes=new Map((source.state._dunlin_turn_outcomes??[]).filter(row=>Number.isSafeInteger(row.message)&&row.message>=0&&Number.isSafeInteger(row.through)&&row.through>=row.message&&row.through<source.state.messages.length).map(row=>[row.message,row]));
 // A pre-metadata journal can still report its actual last terminal outcome.
 if(!source.state.active&&Number.isInteger(source.state.status)&&source.state.status>0){
  const index=source.state.messages.findLastIndex(row=>row.role==='user');
  if(index>=0&&!outcomes.has(index)){const bytes=Buffer.from(String(source.state.detail??'Native turn did not complete'));let end=Math.min(2048,bytes.length);while(end<bytes.length&&(bytes[end]&0xc0)===0x80)end--;outcomes.set(index,{status:source.state.status,detail:bytes.subarray(0,end).toString('utf8'),truncated:end<bytes.length})}
 }
 const reason=()=>{const outcome=outcomes.get(userIndex);if(!outcome)return {kind:'completed'};const message=String(outcome.detail??'Native turn did not complete')+(outcome.truncated?'（错误详情已截短；完整记录见原始会话）':'');return outcome.status===2?{kind:'aborted',message}:{kind:'error',error:{code:'dunlin_status_'+outcome.status,message}}};
 const add=(type,data,surface=false)=>events.push({type,seq:events.length,time:at,data,...(surface?{surfaceOp:'append'}:{})});
 const close=()=>{if(open){if(step)add('step/end',{turn,step});add('turn/end',{turn,reason:reason()});boundaries?.set(turn,lastIndex);open=false}};
 for(const [index,row]of source.state.messages.entries()){
  if(!row||typeof row!=='object'||!['user','assistant','tool','system','developer'].includes(row.role))throw failure('Unsupported canonical message');
  const sourceTime=Number.isSafeInteger(row._dunlin_time)&&row._dunlin_time>0?row._dunlin_time:0;
  const content=typeof row.content==='string'?[{type:'text',text:row.content}]:row.content===null||row.content===undefined?[]:Array.isArray(row.content)?row.content.map(block=>{if(block?.type==='text'&&typeof block.text==='string')return {type:'text',text:block.text};throw failure('Canonical attachment projection required')}):(()=>{throw failure('Invalid canonical content')})();
  const messageId=typeof row._dunlin_message_id==='string'&&row._dunlin_message_id.length>0&&row._dunlin_message_id.length<=128?row._dunlin_message_id:'native-'+chat+'-'+index+'-'+crypto.createHash('sha256').update(JSON.stringify(row)).digest('hex').slice(0,20);
  if(row._dunlin_reference){
   const reference=row._dunlin_reference;if(row.role!=='user'||reference.kind!=='plugin:dsh-checkpoint-rewind'||reference.form!=='notice'||reference.summary!=='rewind')throw failure('Invalid canonical replay reference');
   close();at=sourceTime;add('user/message',{id:messageId,role:'user',source:reference,content},true);lastIndex=index;continue;
  }
  if(row.role==='user'){
   close();at=sourceTime;lastIndex=index;userIndex=index;turn++;step=0;open=true;add('turn/start',{turn});
   let userContent=content;
   if(typeof row._dunlin_display==='string'){
    userContent=[{type:'text',text:row._dunlin_display}];
    // C's model input may include project memory and plugin context. Preserve
    // it as producer-owned reference material, never as words the user said.
    const context=typeof row.content==='string'&&row.content.startsWith(row._dunlin_display)?row.content.slice(row._dunlin_display.length):typeof row.content==='string'?row.content:null;
    if(context)add('user/message',{id:messageId+'-context',role:'user',source:{kind:'plugin:dunlin-c',form:'recall'},content:[{type:'text',text:context}]},true);
   }
   add('user/message',{id:messageId,role:'user',source:{kind:'user'},content:userContent},true);
  }else if(row.role==='assistant'){
   lastIndex=index;
   if(!open){at=sourceTime;turn++;open=true;add('turn/start',{turn})}if(step)add('step/end',{turn,step});at=sourceTime;step++;add('step/start',{turn,step});
   if(typeof row.reasoning_content==='string')content.unshift({type:'reasoning',text:row.reasoning_content});
   if(row.tool_calls!==undefined){if(!Array.isArray(row.tool_calls))throw failure('Invalid canonical tool calls');for(const call of row.tool_calls){if(typeof call?.id!=='string'||typeof call.function?.name!=='string'||typeof call.function?.arguments!=='string')throw failure('Invalid canonical tool call');content.push({type:'tool-call',id:call.id,name:call.function.name,arguments:call.function.arguments})}}
   add('assistant/message',{turn,step,stream:[],message:{id:messageId,role:'assistant',source:{kind:'model',provider:'dunlin-native',model:row._dunlin_profile??'unrecorded-native-model'},content}},true);
   for(const block of content)if(block.type==='tool-call')add('tool/call',{turn,step,callId:block.id,name:block.name,arguments:block.arguments});
  }else if(row.role==='tool'){
   at=sourceTime;
   lastIndex=index;
   if(!open||!step||typeof row.tool_call_id!=='string')throw failure('Canonical tool result has no request');
   add('tool/result',{turn,step,message:{id:messageId,role:'tool',toolCallId:row.tool_call_id,source:{kind:'tool',callId:row.tool_call_id},content}},true);
  }else {at=sourceTime;lastIndex=index;add(row.role+'/message',{message:{id:messageId,role:row.role,source:{kind:row.role==='system'?'system-prompt':'plugin:dunlin-c'},content}},true);}
 }
 // Never pretend that a still-active C turn completed.
 if(open&&!source.state.active)close();
 return events;
}
export function nativeRestoreOptions(chat,source){
 const events=nativeHistoryEvents(chat,source),agent=source.agent;
 if(!agent)return {seed:events,meta:{cwd:source.cwd}};
 const through=source.inheritedThrough;if(!Number.isSafeInteger(through)||through< -1||through>=source.state.messages.length)throw failure('Invalid native inherited boundary');
 const inherited=nativeHistoryEvents(chat,{...source,state:{...source.state,messages:source.state.messages.slice(0,through+1),active:false,status:0}});
 if(inherited.some((event,index)=>JSON.stringify(event)!==JSON.stringify(events[index])))throw failure('Native inherited source no longer matches');
 const prefixLength=inherited.length;
 if(agent.descriptor)inherited.push({type:'subagent/descriptor',seq:inherited.length,time:0,data:agent.descriptor});
 const inheritedEventCount=inherited.length;
 const seed=[...inherited,{type:'session/end-seed',seq:inheritedEventCount,time:0,data:{inherited:true}},...events.slice(prefixLength)].map((event,seq)=>({...event,seq}));
 const meta={cwd:source.cwd,parentSession:agent.parentSession,origin:agent.origin,isSeeded:true,...(Number.isSafeInteger(agent.delegationDepth)?{delegationDepth:agent.delegationDepth}:{}),...(typeof agent.agentPreset==='string'?{agentPreset:agent.agentPreset}:{})};
 return {seed,meta,inheritedEventCount};
}
