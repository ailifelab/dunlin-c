// Projection of the C executor's transient visible-text snapshot into the
// pinned SDK's process-local stream. Never append these frames to a session.
export function refreshNativeLive(root,native,entry){
 const result=native.call('agent.progress',{chat:entry.session.id});
 if(!result?.ok)throw Object.assign(Error(result?.error??'Native progress unavailable'),{code:-32602});
 const live=result.data,agent=entry.agent??{session:entry.session};
 const emit=frame=>root.emit('agent/assistant-stream',{agent,frame});
 if(!live?.active||!entry.source?.state.active||live.through!==entry.source.state.messages.length){
  if(entry.liveAttempt){emit({type:'end',attemptId:entry.liveAttempt});entry.liveAttempt=undefined}return;
 }
 if(typeof live.text!=='string'||Buffer.byteLength(live.text)>65536||typeof live.attemptId!=='string'||live.attemptId.length>90||!Number.isSafeInteger(live.time)||live.time<0)throw Object.assign(Error('Invalid native live snapshot'),{code:-32602});
 const events=entry.session.snapshotEvents(),turn=events.findLast(row=>row.type==='turn/start')?.data.turn;
 if(!Number.isSafeInteger(turn)||turn<0)return;
 const previous=events.findLast(row=>row.type==='step/start'&&row.data.turn===turn)?.data.step??0,step=previous+1;
 // Each snapshot is complete from attempt start. Replacing the transient
 // buffer avoids double deltas after reconnect, dropped polls or engine rebind.
 emit({type:'start',attemptId:live.attemptId,turn,step});
 if(live.text)emit({type:'chunk',attemptId:live.attemptId,turn,step,index:0,time:live.time,chunk:{type:'text-delta',text:live.text}});
 entry.liveAttempt=live.attemptId;
}
