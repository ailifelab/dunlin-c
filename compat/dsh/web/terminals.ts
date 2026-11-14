// Structural host client face consumed by the unchanged sidebar TerminalView.
// PTYs and retained bytes live in the independent C service. There is no shell
// process, agent/model loop or permission decision in this browser adapter.
import {observation} from './workbench'
export function installTerminals(root:any,request:any){
 const models=new Map<string,any>(),lease=crypto.randomUUID();let disposed=false
 const storageKey='dunlin.terminal.bindings.v1';let bindings:Record<string,string>={},bindingError=false
 try{const saved=JSON.parse(localStorage.getItem(storageKey)??'{}');if(!saved||Array.isArray(saved)||Object.values(saved).some(v=>typeof v!=='string'||v.length>64))throw Error('Invalid terminal bindings');bindings=saved}catch{bindingError=true}
 const call=async(method:string,sessionId:string,args:any={})=>{if(disposed)throw Error('Terminal surface closed');const row=await request('native.terminal.'+method,{...args,sessionId,_surface:lease});return row?.value??row}
 const delay=(ms:number)=>new Promise(r=>setTimeout(r,ms))
 function view(sessionId:string,key:string,contentId:string,terminalId?:string,shellPath?:string){
  const identity=JSON.stringify([sessionId,key]),binding=JSON.stringify([sessionId,contentId]);if(models.has(identity))return models.get(identity)
  const state=observation<any>({phase:'idle',writable:false});let id=terminalId??bindings[binding],offset=0,revision=0,epoch=0,mounts=0,timer:number|undefined,refreshing:Promise<void>|undefined,closing=false,polling=false,decoder=new TextDecoder(),queue:Uint8Array[]=[],queuedBytes=0,writing=false
  const publish=(next:any)=>state.set({...state.getSnapshot(),...next})
  const failure=(error:any)=>{if(!disposed&&!closing)publish({phase:'failed',writable:false,error:String(error?.message??error)})}
  const schedule=()=>{clearTimeout(timer);if(!disposed&&!closing&&mounts>0)timer=window.setTimeout(()=>void poll(),150)}
  async function poll(){
   if(polling||!id||closing||disposed||!mounts||state.getSnapshot().render)return
   polling=true;const observed=epoch
   try{const row=await call('read',sessionId,{id,offset});if(closing||disposed||epoch!==observed)return;const bytes=Uint8Array.from(atob(row.base64),c=>c.charCodeAt(0));offset+=bytes.length;const data=decoder.decode(bytes,{stream:true});publish({info:row.info,title:row.info.title,writable:row.writable,phase:row.info.state==='running'?'connected':row.info.state==='failed'?'failed':'closed',...(data?{render:{revision:++revision,frame:{type:'output',sequence:offset,data}}}:{})})}catch(error){if(epoch===observed)failure(error)}finally{polling=false;schedule()}
  }
  async function connect(){
   if(!id||closing||disposed)return
   const observed=++epoch;clearTimeout(timer);publish({phase:'connecting',writable:false,render:undefined});await call('attach',sessionId,{id});if(epoch!==observed||closing||disposed)return;offset=0;decoder=new TextDecoder();let screen='',last:any
   do{last=await call('read',sessionId,{id,offset});if(closing||disposed||epoch!==observed)return;const bytes=Uint8Array.from(atob(last.base64),c=>c.charCodeAt(0));offset+=bytes.length;screen+=decoder.decode(bytes,{stream:true});if(!bytes.length)break}while(offset<last.bytes)
   publish({phase:last.info.state==='running'?'connected':last.info.state==='failed'?'failed':'closed',info:last.info,title:last.info.title,writable:last.writable,render:{revision:++revision,frame:{type:'snapshot',sequence:offset,screen:'\x1bc'+screen,info:last.info}}});schedule()
  }
  async function refresh(){
   if(closing||disposed)return;if(refreshing)return refreshing
   refreshing=(async()=>{if(bindingError)throw Error('Terminal binding storage is corrupt');if(!id&&Object.keys(bindings).length>=512)throw Error('Terminal binding limit');publish({phase:'creating',error:undefined,issue:undefined});const row=await call('open',sessionId,{contentId,...(id?{id}:{}),...(shellPath?{shellPath}:{})});if(closing||disposed)return;id=row.info.id;bindings[binding]=id!;try{localStorage.setItem(storageKey,JSON.stringify(bindings))}catch(error){await call('close',sessionId,{id});throw error}publish({environment:row.environment,info:row.info,title:row.info.title});if(mounts)await connect()})().catch(error=>{failure(error);if(id&&String(error).includes('terminal_operation_refused'))publish({issue:'missingTerminal'})}).finally(()=>{refreshing=undefined});return refreshing
  }
  async function drain(){
   if(writing)return;writing=true
   try{while(queue.length&&!disposed&&!closing){const bytes=queue[0],part=bytes.subarray(0,16384),result=await call('write',sessionId,{id,data:Array.from(part)});if(closing||disposed)return;if(!Number.isInteger(result.written)||result.written<0||result.written>part.length)throw Error('Invalid terminal input receipt');if(!result.written){await delay(10);continue}queuedBytes-=result.written;if(result.written===bytes.length)queue.shift();else queue[0]=bytes.subarray(result.written)}}catch(error){queue=[];queuedBytes=0;failure(error)}finally{writing=false}
  }
  const model={get id(){return id??''},state,mount(){mounts++;if(!refreshing&&state.getSnapshot().phase==='idle')void refresh();else if(id)void connect().catch(failure);return()=>{mounts=Math.max(0,mounts-1);if(!mounts){epoch++;clearTimeout(timer);publish({writable:false});if(id&&!disposed)void call('detach',sessionId,{id}).catch(()=>{})}}},refresh,connect(){void connect().catch(failure)},acknowledge(value:number){if(state.getSnapshot().render?.revision!==value)return;publish({render:undefined});schedule()},write(data:string){if(!state.getSnapshot().writable||closing||disposed)return;const bytes=new TextEncoder().encode(data);if(!bytes.length)return;if(queuedBytes+bytes.length>65536){publish({issue:'inputFull'});return}queuedBytes+=bytes.length;queue.push(bytes);void drain()},resize(cols:number,rows:number){if(!state.getSnapshot().writable||closing||disposed)return;void call('resize',sessionId,{id,cols,rows}).catch(failure)},async close(){if(closing)return;closing=true;clearTimeout(timer);queue=[];queuedBytes=0;publish({phase:'closing',writable:false});await call('close',sessionId,id?{id}:{contentId});delete bindings[binding];localStorage.setItem(storageKey,JSON.stringify(bindings));publish({phase:'closed'})},release(){clearTimeout(timer);if(id&&!disposed)void call('detach',sessionId,{id}).catch(()=>{})}}
  models.set(identity,model);void refresh();return model
 }
 root.provide('webTerminals',{view,close(sessionId:string,key:string,contentId:string,terminalId?:string){const model=models.get(JSON.stringify([sessionId,key]));if(model)void model.close().catch(console.error);else {const binding=JSON.stringify([sessionId,contentId]),id=terminalId??bindings[binding];void call('close',sessionId,id?{id}:{contentId}).then(()=>{delete bindings[binding];localStorage.setItem(storageKey,JSON.stringify(bindings))}).catch(console.error)}},recover:(sessionId:string)=>call('recover',sessionId)})
 root.effect(()=>()=>{for(const model of models.values())model.release();disposed=true;models.clear()})
}
