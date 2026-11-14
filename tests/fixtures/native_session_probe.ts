import path from 'node:path';
export const inject=['dunlin','sessions','sessionPersistence'];
// Test-only observer of the actual SDK session and actual JSONL writer.
export function apply(ctx:any){
 ctx.dunlin.registerCommand(ctx,'probe',async(args:any)=>{
  if(args.target){try{const agent=ctx.get('agents').get(args.target);return {found:!!agent}}catch(error:any){return {nativeError:error.message,code:error.code}}}
  const session=ctx.sessions.get(args.chat??'ui-chat');if(!session)throw Error('Native projection absent');
  if(args.mode==='source-time-bounds'){
   const before=session.snapshotEvents();let rejected=0;
   for(const value of [-1,NaN,Infinity,1.5,Number.MAX_SAFE_INTEGER+1])try{session.append('session/title',{title:'Owned invalid source time',messageSeqs:[],source:{kind:'user'}},{[Symbol.for('dunlin.native.sourceTime')]:value})}catch{rejected++}
   return {rejected,unchanged:JSON.stringify(session.snapshotEvents())===JSON.stringify(before)};
  }
  if(args.mode==='cwd'){
   const seed=[...session.snapshotEvents()],count=seed.length;seed.push({type:'session/end-seed',seq:count,time:seed.at(-1)?.time??0,data:{inherited:true}});
   const outside=path.dirname(session.header.cwd);
   try{const lease=await ctx.get('agents').create({sessionId:'owned-cwd-probe',seed,inheritedEventCount:count,meta:{parentSession:session.id,origin:'subagent',isSeeded:true,cwd:outside}});const headerCwd=lease.agent.session.header.cwd;await lease.dispose();return {accepted:true,headerCwd,parentCwd:session.header.cwd,outside}}
   catch(error:any){return {accepted:false,nativeError:error.message,code:error.code,parentCwd:session.header.cwd,outside}}
  }
  await ctx.sessions.flush(session);const handle=await ctx.sessionPersistence.open(session.id,'read');
  try{const stored=await handle.read();return {id:session.id,header:handle.header,inheritedEventCount:session.inheritedEventCount,storedInheritedEventCount:handle.inheritedEventCount,live:session.snapshotEvents(),stored:stored.events,source:'derived-from-c'}}finally{await handle.close()}
 });
}
