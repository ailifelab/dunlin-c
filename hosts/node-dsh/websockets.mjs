import {Duplex} from 'node:stream';
import {createServer} from 'node:http';
import {randomUUID} from 'node:crypto';
import WebSocket from 'ws';

// Actual HTTP upgrade and RFC6455 implementations over a private duplex pair.
// No listener/port is opened. The bridge can address only the owning route.
function pair(){
 let left,right;
 const make=other=>new Duplex({read(){},write(data,encoding,next){const peer=other();if(peer.destroyed)return next(Error('socket closed'));peer.push(Buffer.from(data));next()},final(next){other().push(null);next()},destroy(error,next){const peer=other();if(!peer.destroyed)peer.destroy();next(error)}});
 left=make(()=>right);right=make(()=>left);
 for(const stream of [left,right]){stream.setNoDelay=()=>stream;stream.setKeepAlive=()=>stream;stream.setTimeout=()=>stream;stream.remoteAddress='127.0.0.1'}
 return [left,right];
}
export function registerUpgrade(scope,route,{owned,commands,contributions,admission,observe}){
 if(typeof route?.path!=='string'||!/^\/[A-Za-z0-9_./-]{1,256}$/.test(route.path)||typeof route.handler!=='function')throw Error('unsupported upgrade route');
 const channels=new Map();
 const server=createServer();
 server.on('upgrade',(req,socket,head)=>{req[admission]=true;Promise.resolve().then(()=>route.handler(req,socket,head)).catch(()=>socket.destroy())});
 const command='ws:'+route.path;
 const handler=async args=>{
  if(args.op==='closeSurface'){for(const [id,row]of channels)if(row.surface===args._surface){row.ws.terminate();channels.delete(id)}return {closed:true}}
  if(args.op==='open'){
   if(channels.size>=16)throw Error('websocket channel limit');
   const url=new URL(args.url ?? route.path,'http://127.0.0.1');if(url.origin!=='http://127.0.0.1'||url.pathname!==route.path)throw Error('upgrade ownership mismatch');
   const [client,socket]=pair();server.emit('connection',socket);
   const ws=new WebSocket('ws://127.0.0.1'+url.pathname+url.search,{createConnection:()=>client,handshakeTimeout:5000,headers:{origin:'http://127.0.0.1'}});
   const id=randomUUID(),row={ws,events:[],bytes:0,error:null,surface:args._surface,watched:[],cursor:0};channels.set(id,row);
   ws.on('message',(data,binary)=>{row.bytes+=data.length;if(row.bytes>262144||row.events.length>=128){row.error='websocket queue overflow';ws.terminate();return}row.events.push(binary?{type:'message',base64:data.toString('base64')}:{type:'message',text:data.toString()})});
   ws.on('close',(code,reason)=>row.events.push({type:'close',code,reason:reason.toString().slice(0,128)}));
   ws.on('error',()=>{row.error='websocket transport error'});
   await new Promise((resolve,reject)=>{ws.once('open',resolve);ws.once('error',reject);ws.once('close',()=>reject(Error('websocket closed before opening')))}).catch(error=>{channels.delete(id);ws.terminate();throw error});
   return {id};
  }
  const row=channels.get(args.id);if(!row||row.surface!==args._surface)throw Object.assign(Error('websocket channel unavailable'),{code:-32602});
  if(args.op==='close'){row.ws.terminate();channels.delete(args.id);return {closed:true}}
  if(row.error)throw Error(row.error);
  if(args.op==='send'){const value=args.base64===undefined?String(args.text ?? ''):Buffer.from(args.base64,'base64');if(Buffer.byteLength(value)>65536)throw Error('websocket message exceeds limit');await new Promise((resolve,reject)=>row.ws.send(value,error=>error?reject(error):resolve()));
   // The pinned SideChat route watches canonical C children. Its own server
   // still owns subscription semantics and emits the actual invalidations.
   if(route.path==='/sidebar/ws/sidechat'&&typeof value==='string'){
    let frame;try{frame=JSON.parse(value)}catch{}
    if(frame?.op==='watch'){row.watched=[...new Set((Array.isArray(frame.threadIds)?frame.threadIds:[]).filter(id=>typeof id==='string'&&id.length>0&&id.length<=64).slice(0,64))];row.cursor=0}
   }
   return {sent:true}}
  if(args.op==='read'){
   // Refresh one watched source per admitted read, round-robin. Reading one
   // complete bounded source plus the owning root stays within FD3's callback
   // budget even for large histories; there are no idle native callbacks.
   if(observe&&row.watched.length)await observe(row.watched[row.cursor++%row.watched.length]);
   const events=row.events.splice(0);row.bytes=0;return {events}
  }
  throw Error('unsupported websocket operation');
 };
 const key=owned(scope,command,handler,commands);
 const contribution=owned(scope,command,{kind:'command',title:route.path,command},contributions);
 let disposed=false;const dispose=()=>{if(disposed)return;disposed=true;for(const row of channels.values())row.ws.terminate();channels.clear();commands.delete(key);contributions.delete(contribution);server.removeAllListeners()};scope.effect(()=>dispose);return dispose;
}
