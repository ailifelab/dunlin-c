// Only the exact sessionQuery read face consumed by the pinned Project plugin.
// C source is the authority; never enumerate arbitrary JSONL files or run agents.
import {Service} from '@deepseek-ai/cordis';
import {Session,SESSION_FORMAT_VERSION} from '@deepseek-ai/dsh-session';
import {readNativeSource,nativeRestoreOptions} from './native_sessions.mjs';
export function installNativeQuery(root,{native}){
 class NativeSessionQuery extends Service {
  constructor(ctx){super(ctx,'sessionQuery')}
  async readSession(id){
   if(typeof id!=='string'||!id||id.length>64)throw Object.assign(Error('Native session identity required'),{code:-32602});
   // Fresh C admission is required even if this engine previously projected id
   // for another plugin/root. A cached SDK session is never an access grant.
   const source=readNativeSource(native,id);
   const restored=nativeRestoreOptions(id,source);
   // A detached, replay-validated SDK object avoids creating live sessions or
   // asynchronous writers for a read. The C source has no historical session
   // birth timestamp: zero is explicit unknown, never the cache creation time.
   const header={version:SESSION_FORMAT_VERSION,id,createdAt:0,isSeeded:false,...restored.meta};
   const session=Session.create(id,restored.seed,header,restored.inheritedEventCount??0);
   return {session:structuredClone(session.header),inheritedEventCount:session.inheritedEventCount,events:structuredClone(session.snapshotEvents())};
  }
  async listEvents(id){
   const {events}=await this.readSession(id);
   // Preserve unknown zero. The pinned Project calendar repair excludes it
   // and reports partial/unknown history instead of fabricating 1970 activity.
   return events.map(({seq,type,time,surfaceOp})=>({sessionId:id,seq,type,time,surface:surfaceOp?'current':'log-only'}));
  }
 }
 return new NativeSessionQuery(root);
}
