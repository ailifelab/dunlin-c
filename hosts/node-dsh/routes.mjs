import {Service} from '@deepseek-ai/cordis';
import {Readable,Writable} from 'node:stream';
import {StringDecoder} from 'node:string_decoder';
import {randomUUID,createHash} from 'node:crypto';
import {registerUpgrade} from './websockets.mjs';
import {httpJobs} from './http_jobs.mjs';
// Transport only: requests come from the C-owned plugin invocation, never an
// open HTTP port. No caller-supplied Host/Origin/authorization headers are used.
export function installRoutes(root,{owned,commands,contributions,observe}){
 const admission=Symbol('native route'),jobs=httpJobs();
 const routes=new Map(),bodies=new Map(),uploads=new Map(),streams=new Map();let bodyBytes=0,uploadBytes=0;
 const prune=()=>{for(const [id,row]of streams)if(row.expires<Date.now()){row.close();streams.delete(id)}for(const [id,row]of uploads)if(row.expires<Date.now()){uploadBytes-=row.size;uploads.delete(id)}for(const [id,row]of bodies)if(row.expires<Date.now()){bodyBytes-=row.bytes.length;bodies.delete(id)}};
 function response(owner,status,headers,bytes){
  if(bytes.length>262144){
   prune();if(bodyBytes+bytes.length>67108864)return {status:503,headers:{},body:{error:'native_response_cache_full'}};
   const bodyHandle=randomUUID();bodies.set(bodyHandle,{owner,bytes,expires:Date.now()+60000});bodyBytes+=bytes.length;
   return {status,headers,bodyHandle,bytes:bytes.length};
  }
  let body;try{body=bytes.length?JSON.parse(bytes.toString('utf8')):null}catch{body=undefined}
  return {status,headers,...(body===undefined?{bodyBase64:bytes.toString('base64')}:{body})};
 }
 root.provide('connection',Object.freeze({trustedHosts:['127.0.0.1','localhost'],requestRejection:req=>req?.[admission]===true?undefined:{status:403,message:'Native bridge required'}}));
 class Routes extends Service {
  constructor(ctx){super(ctx,'webServer')}
  registerUpgrade(route){return registerUpgrade(this.ctx,route,{owned,commands,contributions,admission,observe})}
  register(route){
   if(!['exact','prefix'].includes(route?.kind)||typeof route.path!=='string'||!/^\/[a-zA-Z0-9_./-]{1,256}$/.test(route.path)||typeof route.handler!=='function')throw Error('unsupported route');
   const command='route:'+route.path;
   const key=owned(this.ctx,command,args=>{
    const method=args.method ?? 'GET';if(!['GET','HEAD','POST','PUT','PATCH','DELETE'].includes(method))throw Error('unsupported method');
    const url=new URL(args.url ?? route.path,'http://127.0.0.1');
    if(url.origin!=='http://127.0.0.1'||(route.kind==='exact'?url.pathname!==route.path:!url.pathname.startsWith(route.path)))throw Error('route ownership mismatch');
    const body=Buffer.isBuffer(args.bodyBuffer)?args.bodyBuffer:args.bodyBase64===undefined?Buffer.from(JSON.stringify(args.body ?? {})):Buffer.from(args.bodyBase64,'base64');if(body.length>(Buffer.isBuffer(args.bodyBuffer)?16777216:131072))throw Error('route body exceeds limit');
    const applicationHeaders={};const profile=args.headers?.['x-dsh-vault-profile'];if(typeof profile==='string'&&/^[A-Za-z0-9_-]{8,128}$/.test(profile))applicationHeaders['x-dsh-vault-profile']=profile;
    const req=Readable.from(['GET','HEAD'].includes(method)?[]:[body]);Object.assign(req,{method,url:url.pathname+url.search,headers:{...applicationHeaders,'content-type':typeof args.contentType==='string'&&/^(?:application\/(?:json|octet-stream|x-www-form-urlencoded)|image\/(?:png|jpeg|webp|gif)|multipart\/form-data; boundary=[A-Za-z0-9_.-]{1,120})$/.test(args.contentType)?args.contentType:'application/json',host:'127.0.0.1',origin:'http://127.0.0.1'},socket:{remoteAddress:'127.0.0.1'}});req[admission]=true;
    let abort;const operation=new Promise((resolve,reject)=>{
     let size=0,stream;const chunks=[];const headers={};
     // The private asynchronous owner retains the C running intent.
     const timeout=Number.isSafeInteger(args._timeoutMs)?Math.max(1000,Math.min(3600000,args._timeoutMs)):150000;
     const timer=setTimeout(()=>{req.emit('aborted');req.destroy();res.destroy(Error('Plugin HTTP operation timed out'))},timeout);
     const res=new Writable({write(chunk,encoding,next){if(stream){if(stream.bytes+chunk.length>262144){stream.close();return next()}stream.chunks.push(stream.decoder.write(chunk));stream.bytes+=chunk.length;return next()}size+=chunk.length;if(size>16777216)return next(Error('route response exceeds limit'));chunks.push(Buffer.from(chunk));next()}});
     abort=()=>{req.emit('aborted');req.destroy();res.destroy(Error('Plugin HTTP operation cancelled'))};
     res.statusCode=200;res.headersSent=false;
     res.setHeader=(name,value)=>{headers[String(name).toLowerCase()]=String(value);return res};
     res.getHeader=name=>headers[String(name).toLowerCase()];
     res.removeHeader=name=>{delete headers[String(name).toLowerCase()]};
     res.writeHead=(status,values={})=>{res.statusCode=status;res.headersSent=true;for(const [name,value]of Object.entries(values))res.setHeader(name,value);return res};
     const startStream=()=>{
      if(stream||!String(headers['content-type']).startsWith('text/event-stream'))return;
      if(streams.size>=16)throw Object.assign(Error('Stream limit'),{code:-32602});
      const id=randomUUID();stream={owner:commands.get(key)?.owner,surface:args._surface,decoder:new StringDecoder('utf8'),chunks:[],bytes:0,closed:false,expires:Date.now()+60000,close(){if(stream.closed)return;stream.closed=true;req.destroy();res.end()}};streams.set(id,stream);clearTimeout(timer);resolve({status:res.statusCode,headers,eventHandle:id});
     };
     const originalWriteHead=res.writeHead;res.writeHead=(...args)=>{originalWriteHead(...args);startStream();return res};
     res.flushHeaders=()=>{res.headersSent=true;startStream()};
     res.once('finish',()=>{clearTimeout(timer);if(stream){const tail=stream.decoder.end();if(tail)stream.chunks.push(tail);stream.closed=true;return}const bytes=Buffer.concat(chunks);resolve(response(commands.get(key)?.owner,res.statusCode,headers,bytes))});
     res.once('error',error=>{clearTimeout(timer);reject(error)});
     Promise.resolve().then(()=>route.handler(req,res)).catch(error=>{clearTimeout(timer);reject(error)});
    });operation.abort=()=>abort?.();return operation;
   },commands);
   routes.set(key,{kind:route.kind,path:route.path,invoke:commands.get(key).value});
   const contribution=owned(this.ctx,command,{kind:'command',title:route.path,command},contributions);
   let disposed=false;const dispose=async()=>{if(disposed)return;disposed=true;await api.close(commands.get(key)?.owner,undefined,true);for(const [id,row]of bodies)if(row.owner===commands.get(key)?.owner){bodyBytes-=row.bytes.length;bodies.delete(id)}commands.delete(key);routes.delete(key);contributions.delete(contribution)};this.ctx.effect(()=>dispose);return dispose;
  }
 }
 new Routes(root);
 const api={pending:(plugin,args)=>jobs.read(plugin,args),events(plugin,args){prune();const row=streams.get(args.id);if(!row||row.owner!==plugin||row.surface!==args._surface)throw Object.assign(Error('Stream unavailable'),{code:-32602});if(args.release){row.close();streams.delete(args.id);return {released:true}}row.expires=Date.now()+60000;const chunks=row.chunks.splice(0);row.bytes=0;return {chunks,closed:row.closed}},async close(plugin,surface,all=false){
  await jobs.close(plugin,surface,all);
  for(const [id,row]of streams)if(row.owner===plugin&&(all||row.surface===surface)){row.close();streams.delete(id)}
  for(const [id,row]of uploads)if(row.owner===plugin&&(all||row.surface===surface)){uploadBytes-=row.size;uploads.delete(id)}
 },async upload(plugin,operation,args){
  prune();
  if(operation==='begin'){
   if(!Number.isSafeInteger(args.bytes)||args.bytes<1||args.bytes>16777216||uploadBytes+args.bytes>33554432||[...uploads.values()].filter(r=>r.owner===plugin).length>=4||typeof args.url!=='string'||args.url.length>4096||typeof args.sha256!=='string'||!/^[a-f0-9]{64}$/.test(args.sha256))throw Object.assign(Error('Invalid or excessive upload'),{code:-32602});
   const url=new URL(args.url,'http://127.0.0.1');if(url.origin!=='http://127.0.0.1'||!['POST','PUT','PATCH'].includes(args.method))throw Object.assign(Error('Invalid upload route'),{code:-32602});
   const id=randomUUID();uploads.set(id,{owner:plugin,surface:args._surface,size:args.bytes,offset:0,bytes:Buffer.alloc(args.bytes),args:{url:url.pathname+url.search,method:args.method,contentType:args.contentType,headers:args.headers},sha256:args.sha256,expires:Date.now()+60000});uploadBytes+=args.bytes;return {id};
  }
  const row=uploads.get(args.id);if(!row||row.owner!==plugin||row.surface!==args._surface)throw Object.assign(Error('Upload unavailable'),{code:-32602});
  if(operation==='release'){uploadBytes-=row.size;uploads.delete(args.id);return {released:true}}
  if(operation==='append'){
   if(args.offset!==row.offset||typeof args.base64!=='string'||args.base64.length>98304||!/^[A-Za-z0-9+/]*={0,2}$/.test(args.base64))throw Object.assign(Error('Invalid upload chunk'),{code:-32602});
   const bytes=Buffer.from(args.base64,'base64');if(!bytes.length||row.offset+bytes.length>row.size)throw Object.assign(Error('Upload overflow'),{code:-32602});bytes.copy(row.bytes,row.offset);row.offset+=bytes.length;row.expires=Date.now()+60000;return {received:row.offset};
  }
  if(operation==='commit'){
   if(row.offset!==row.size||createHash('sha256').update(row.bytes).digest('hex')!==row.sha256)throw Object.assign(Error('Upload incomplete or altered'),{code:-32602});
   uploadBytes-=row.size;uploads.delete(args.id);return api.request(plugin,{...row.args,bodyBuffer:row.bytes,_surface:row.surface});
  }
  throw Object.assign(Error('Invalid upload operation'),{code:-32602});
 },body(plugin,args){
  prune();const row=bodies.get(args.id);if(!row||row.owner!==plugin)throw Object.assign(Error('Body handle unavailable'),{code:-32602});
  if(args.release===true){bodyBytes-=row.bytes.length;bodies.delete(args.id);return {released:true}}
  if(!Number.isSafeInteger(args.offset)||args.offset<0||args.offset>row.bytes.length)throw Object.assign(Error('Invalid body offset'),{code:-32602});
  return {base64:row.bytes.subarray(args.offset,args.offset+98304).toString('base64'),total:row.bytes.length};
 },async request(plugin,args){
  const url=new URL(args.url ?? '/','http://127.0.0.1');if(url.origin!=='http://127.0.0.1')throw Error('External route refused');
  const found=[...routes].filter(([key,r])=>key.startsWith(plugin+':')&&(r.kind==='exact'?r.path===url.pathname:url.pathname.startsWith(r.path))).sort((a,b)=>b[1].path.length-a[1].path.length)[0];
  return found?jobs.start(plugin,args,()=>found[1].invoke(args)):{status:404,body:{error:'route_not_found'}};
 }};return api;
}
