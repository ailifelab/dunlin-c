import {Service} from '@deepseek-ai/cordis';
import {Readable,Writable} from 'node:stream';
import {randomUUID} from 'node:crypto';
import {registerUpgrade} from './websockets.mjs';
// Transport only: requests come from the C-owned plugin invocation, never an
// open HTTP port. No caller-supplied Host/Origin/authorization headers are used.
export function installRoutes(root,{owned,commands,contributions}){
 const admission=Symbol('native route');
 const routes=new Map(),bodies=new Map();let bodyBytes=0;
 const prune=()=>{for(const [id,row]of bodies)if(row.expires<Date.now()){bodyBytes-=row.bytes.length;bodies.delete(id)}};
 function response(owner,status,headers,bytes){
  if(bytes.length>262144){
   prune();if(bodyBytes+bytes.length>16777216)return {status:503,headers:{},body:{error:'native_response_cache_full'}};
   const bodyHandle=randomUUID();bodies.set(bodyHandle,{owner,bytes,expires:Date.now()+60000});bodyBytes+=bytes.length;
   return {status,headers,bodyHandle,bytes:bytes.length};
  }
  let body;try{body=bytes.length?JSON.parse(bytes.toString('utf8')):null}catch{body=undefined}
  return {status,headers,...(body===undefined?{bodyBase64:bytes.toString('base64')}:{body})};
 }
 root.provide('connection',Object.freeze({trustedHosts:['127.0.0.1','localhost'],requestRejection:req=>req?.[admission]===true?undefined:{status:403,message:'Native bridge required'}}));
 class Routes extends Service {
  constructor(ctx){super(ctx,'webServer')}
  registerUpgrade(route){return registerUpgrade(this.ctx,route,{owned,commands,contributions,admission})}
  register(route){
   if(!['exact','prefix'].includes(route?.kind)||typeof route.path!=='string'||!/^\/[a-zA-Z0-9_./-]{1,256}$/.test(route.path)||typeof route.handler!=='function')throw Error('unsupported route');
   const command='route:'+route.path;
   const key=owned(this.ctx,command,async args=>{
    const method=args.method ?? 'GET';if(!['GET','HEAD','POST','PUT','PATCH','DELETE'].includes(method))throw Error('unsupported method');
    const url=new URL(args.url ?? route.path,'http://127.0.0.1');
    if(url.origin!=='http://127.0.0.1'||(route.kind==='exact'?url.pathname!==route.path:!url.pathname.startsWith(route.path)))throw Error('route ownership mismatch');
    const body=args.bodyBase64===undefined?Buffer.from(JSON.stringify(args.body ?? {})):Buffer.from(args.bodyBase64,'base64');if(body.length>131072)throw Error('route body exceeds limit');
    const req=Readable.from(['GET','HEAD'].includes(method)?[]:[body]);Object.assign(req,{method,url:url.pathname+url.search,headers:{'content-type':args.contentType==='application/octet-stream'?'application/octet-stream':'application/json',host:'127.0.0.1',origin:'http://127.0.0.1'},socket:{remoteAddress:'127.0.0.1'}});req[admission]=true;
    return await new Promise((resolve,reject)=>{
     let size=0;const chunks=[];const headers={};
     // Kill on timeout so the C call ledger retains unknown effects and does not replay.
     const timer=setTimeout(()=>process.exit(74),25000);
     const res=new Writable({write(chunk,encoding,next){size+=chunk.length;if(size>16777216)return next(Error('route response exceeds limit'));chunks.push(Buffer.from(chunk));next()}});
     res.statusCode=200;res.headersSent=false;
     res.setHeader=(name,value)=>{headers[String(name).toLowerCase()]=String(value);return res};
     res.getHeader=name=>headers[String(name).toLowerCase()];
     res.removeHeader=name=>{delete headers[String(name).toLowerCase()]};
     res.writeHead=(status,values={})=>{res.statusCode=status;res.headersSent=true;for(const [name,value]of Object.entries(values))res.setHeader(name,value);return res};
     res.flushHeaders=()=>{res.headersSent=true};
     res.once('finish',()=>{clearTimeout(timer);const bytes=Buffer.concat(chunks);resolve(response(commands.get(key)?.owner,res.statusCode,headers,bytes))});
     res.once('error',error=>{clearTimeout(timer);reject(error)});
     Promise.resolve().then(()=>route.handler(req,res)).catch(error=>{clearTimeout(timer);reject(error)});
    });
   },commands);
   routes.set(key,{kind:route.kind,path:route.path,invoke:commands.get(key).value});
   const contribution=owned(this.ctx,command,{kind:'command',title:route.path,command},contributions);
   let disposed=false;const dispose=()=>{if(disposed)return;disposed=true;for(const [id,row]of bodies)if(row.owner===commands.get(key)?.owner){bodyBytes-=row.bytes.length;bodies.delete(id)}commands.delete(key);routes.delete(key);contributions.delete(contribution)};this.ctx.effect(()=>dispose);return dispose;
  }
 }
 new Routes(root);
 return {body(plugin,args){
  prune();const row=bodies.get(args.id);if(!row||row.owner!==plugin)throw Object.assign(Error('Body handle unavailable'),{code:-32602});
  if(args.release===true){bodyBytes-=row.bytes.length;bodies.delete(args.id);return {released:true}}
  if(!Number.isSafeInteger(args.offset)||args.offset<0||args.offset>row.bytes.length)throw Object.assign(Error('Invalid body offset'),{code:-32602});
  return {base64:row.bytes.subarray(args.offset,args.offset+98304).toString('base64'),total:row.bytes.length};
 },async request(plugin,args){
  const url=new URL(args.url ?? '/','http://127.0.0.1');if(url.origin!=='http://127.0.0.1')throw Error('External route refused');
  const found=[...routes].filter(([key,r])=>key.startsWith(plugin+':')&&(r.kind==='exact'?r.path===url.pathname:url.pathname.startsWith(r.path))).sort((a,b)=>b[1].path.length-a[1].path.length)[0];
  return found?found[1].invoke(args):{status:404,body:{error:'route_not_found'}};
 }};
}
