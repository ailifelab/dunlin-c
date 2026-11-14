// Owner-bound fetch/WebSocket bridge, shared by native WebKit and DOM tests.
import {sha256} from '@noble/hashes/sha2.js'
export type NativeRequest=(method:string,args:Record<string,unknown>)=>Promise<any>
export function installTransport(request:NativeRequest){
 const originalFetch=globalThis.fetch,OriginalSocket=globalThis.WebSocket,OriginalSource=globalThis.EventSource,originalSubmit=HTMLFormElement.prototype.submit,originalAnchorClick=HTMLAnchorElement.prototype.click
 const channels=new Set<any>(),pending=new Set<string>(),blobs=new Map<string,Blob>();let blobBytes=0;const originalCreate=URL.createObjectURL,originalRevoke=URL.revokeObjectURL;let closed=false
 const clipboardDescriptor=Object.getOwnPropertyDescriptor(navigator,'clipboard')
 const clipboard=Object.freeze({async writeText(text:string){if(closed||typeof text!=='string'||text.includes('\0')||new TextEncoder().encode(text).length>65536)throw Error('Invalid or closed clipboard write');await request('native.clipboard.write',{text})}})
 Object.defineProperty(navigator,'clipboard',{configurable:true,value:clipboard})
 URL.createObjectURL=function(blob:Blob){if(closed||!(blob instanceof Blob)||blob.size>16777216||blobs.size>=256||blobBytes+blob.size>67108864)throw Error('Owned Blob limit');const url=originalCreate.call(URL,blob);blobs.set(url,blob);blobBytes+=blob.size;return url}
 URL.revokeObjectURL=function(url:string){const blob=blobs.get(url);if(blob){blobBytes-=blob.size;blobs.delete(url)}originalRevoke.call(URL,url)}
 async function terminal(value:any,signal?:AbortSignal){
  if(!value.pendingHandle)return value
  const id=value.pendingHandle;pending.add(id)
  try{
   for(;;){
    if(closed||signal?.aborted)throw new DOMException('Aborted','AbortError')
    const row=await request('http.pending',{id})
    if(row.ready)return row.response
    await new Promise<void>((resolve,reject)=>{
     const abort=()=>{clearTimeout(timer);signal?.removeEventListener('abort',abort);reject(new DOMException('Aborted','AbortError'))}
     const timer=setTimeout(()=>{signal?.removeEventListener('abort',abort);resolve()},200)
     signal?.addEventListener('abort',abort,{once:true});if(signal?.aborted)abort()
    })
   }
  }finally{pending.delete(id);await request('http.pending',{id,release:true}).catch(()=>{})}
 }
 const base64=(bytes:Uint8Array)=>{const chunks=[];for(let i=0;i<bytes.length;i+=32768)chunks.push(String.fromCharCode(...bytes.subarray(i,i+32768)));return btoa(chunks.join(''))}
 function sameOwner(input:string|URL){const url=new URL(String(input),location.href);if(url.protocol!==location.protocol||url.host!==location.host||url.username||url.password)throw Error('External plugin URL refused');return url.pathname+url.search}
 async function responseBytes(value:any){
  if(value.bodyHandle){const parts:Uint8Array[]=[];try{if(value.bytes>16777216)throw Error('Response too large');for(let offset=0;offset<value.bytes;offset+=98304){const part=await request('http.body',{id:value.bodyHandle,offset});parts.push(Uint8Array.from(atob(part.base64),x=>x.charCodeAt(0)))}}finally{await request('http.body',{id:value.bodyHandle,release:true})}return new Blob(parts)}
  return value.bodyBase64?Uint8Array.from(atob(value.bodyBase64),x=>x.charCodeAt(0)):JSON.stringify(value.body)
 }
 globalThis.fetch=async(input:any,init:any={})=>{
  if(closed)throw Error('Plugin surface closed');if(init.signal?.aborted)throw new DOMException('Aborted','AbortError')
  const address=typeof input==='string'||input instanceof URL?String(input):input.url,parsed=new URL(address,location.href),method=init.method??(input instanceof Request?input.method:'GET')
  if(['data:','blob:'].includes(parsed.protocol)){
   if(method!=='GET')throw Error('Local resource method refused')
   if(parsed.protocol==='blob:'){const blob=blobs.get(parsed.href);if(!blob)throw Error('Foreign or revoked Blob URL refused');return new Response(blob,{headers:{'content-type':blob.type||'application/octet-stream'}})}
   const at=address.indexOf(',');if(at<0||at>256||address.length>50332000)throw Error('Invalid or excessive data URL');const meta=address.slice(5,at),encoded=address.slice(at+1).split('#',1)[0];let bytes:Uint8Array
   try{if(/;base64$/i.test(meta)){if(encoded.length>22370000)throw Error('Excessive base64');bytes=Uint8Array.from(atob(encoded),c=>c.charCodeAt(0))}else{const buffer=new Uint8Array(Math.min(encoded.length*3,16777217)),encoder=new TextEncoder(),pattern=/%([0-9a-f]{2})/gi;let at=0,used=0,match;const append=(text:string)=>{const result=encoder.encodeInto(text,buffer.subarray(used));if(result.read!==text.length)throw Error('Excessive data');used+=result.written};while((match=pattern.exec(encoded))){append(encoded.slice(at,match.index));if(used>=buffer.length)throw Error('Excessive data');buffer[used++]=parseInt(match[1],16);at=match.index+3}append(encoded.slice(at));bytes=buffer.subarray(0,used)}}catch{throw Error('Invalid or excessive data URL')}
   if(bytes.length>16777216)throw Error('Data URL exceeds limit');return new Response(bytes,{headers:{'content-type':meta.replace(/;base64$/i,'')||'text/plain;charset=US-ASCII'}})
  }
  // Present a policy denial as an HTTP result. No network request is issued.
  // Many plugins use response.ok to degrade individual optional features.
  if ((parsed.protocol==='https:'||parsed.protocol==='http:')&&(parsed.protocol!==location.protocol||parsed.host!==location.host))
   return new Response('{"error":"external_plugin_network_denied"}',{status:403,headers:{'content-type':'application/json'}})
  const url=sameOwner(address);if(url.startsWith('/dunlin-assets/tldraw/5.3.0/')){if(method!=='GET')throw Error('Static asset method refused');const value=await request('native.asset.read',{path:parsed.pathname});return new Response(Uint8Array.from(atob(value.base64),c=>c.charCodeAt(0)),{headers:{'content-type':value.type}})}
  let body:any={};let bodyBase64:string|undefined,contentType:string|undefined;
  const headers=new Headers(init.headers??{});const profile=headers.get('x-dsh-vault-profile');const safeHeaders=profile&&/^[A-Za-z0-9_-]{8,128}$/.test(profile)?{'x-dsh-vault-profile':profile}:{};const raw=init.body
  if(raw instanceof URLSearchParams){const bytes=new TextEncoder().encode(raw.toString());if(bytes.length>16777216)throw Error('Upload exceeds 16 MiB');bodyBase64=base64(bytes);contentType='application/x-www-form-urlencoded'}
  else if(raw instanceof FormData){const encoded=new Response(raw);contentType=encoded.headers.get('content-type')??undefined;const bytes=new Uint8Array(await encoded.arrayBuffer());if(bytes.length>16777216)throw Error('Upload exceeds 16 MiB');bodyBase64=base64(bytes)}
  else if(raw!=null){if(typeof raw==='string'){try{body=JSON.parse(raw)}catch{bodyBase64=btoa(unescape(encodeURIComponent(raw)));contentType='application/octet-stream'}}else if(raw instanceof Blob||raw instanceof ArrayBuffer||ArrayBuffer.isView(raw)){const bytes=raw instanceof Blob?new Uint8Array(await raw.arrayBuffer()):ArrayBuffer.isView(raw)?new Uint8Array(raw.buffer,raw.byteOffset,raw.byteLength):new Uint8Array(raw);if(bytes.length>16777216)throw Error('Upload exceeds 16 MiB');bodyBase64=base64(bytes);const type=headers.get('content-type')??(raw instanceof Blob?raw.type:'');contentType=/^image\/(?:png|jpeg|webp|gif)$/.test(type)?type:'application/octet-stream'}else throw Error('Unsupported request body')}
  if(raw!=null&&!bodyBase64){const bytes=new TextEncoder().encode(JSON.stringify(body));if(bytes.length>73728){if(bytes.length>16777216)throw Error('Request exceeds 16 MiB');bodyBase64=base64(bytes);contentType='application/json'}}
  let value;
  if(bodyBase64&&bodyBase64.length>98304){
   const bytes=Uint8Array.from(atob(bodyBase64),x=>x.charCodeAt(0));const digest=Array.from(sha256(bytes),b=>b.toString(16).padStart(2,'0')).join('');const upload=await request('http.upload.begin',{url,method:init.method??'POST',contentType,bytes:bytes.length,sha256:digest,headers:safeHeaders});
   try{for(let offset=0;offset<bytes.length;offset+=49152){if(init.signal?.aborted)throw new DOMException('Aborted','AbortError');await request('http.upload.append',{id:upload.id,offset,base64:base64(bytes.subarray(offset,offset+49152))})}value=await request('http.upload.commit',{id:upload.id})}finally{await request('http.upload.release',{id:upload.id}).catch(()=>{})}
  }else value=await request('http',{method:init.method??'GET',url,body,headers:safeHeaders,...(bodyBase64?{bodyBase64,contentType}:{})});value=await terminal(value,init.signal);if(init.signal?.aborted)throw new DOMException('Aborted','AbortError');
  return new Response([204,205,304].includes(value.status)?null:await responseBytes(value),{status:value.status,headers:value.headers})
 }
 async function save(blob:Blob,name:string){
  if(blob.size>16777216)throw Error('Download exceeds the desktop 16 MiB limit')
  const row=await request('download.begin',{name,bytes:blob.size});const bytes=new Uint8Array(await blob.arrayBuffer())
  try{for(let offset=0;offset<bytes.length;offset+=49152){if(closed)throw Error('Plugin surface closed');await request('download.append',{id:row.id,offset,base64:base64(bytes.subarray(offset,offset+49152))})}return await request('download.finish',{id:row.id})}
  finally{await request('download.cancel',{id:row.id}).catch(()=>{})}
 }
 const downloadError=(error:any)=>{if(!closed)void request('download.error',{message:String(error?.message??error).slice(0,300)}).catch(()=>{})}
 const downloadAnchor=async(anchor:HTMLAnchorElement)=>{const url=new URL(anchor.href,location.href);const response=await globalThis.fetch(['blob:','data:'].includes(url.protocol)?url.href:sameOwner(url));if(!response.ok)throw Error('Download failed ('+response.status+')');await save(await response.blob(),anchor.download||'download')}
 const downloadClick=(event:Event)=>{const anchor=(event.target as Element)?.closest?.('a[download]') as HTMLAnchorElement|null;if(!anchor)return;event.preventDefault();void downloadAnchor(anchor).catch(downloadError)}
 // SDK canvas exporters click an unattached anchor and revoke its Blob in the
 // same tick. Document capture never sees it; admit its bytes before returning.
 HTMLAnchorElement.prototype.click=function(){if(this.hasAttribute('download')){void downloadAnchor(this).catch(downloadError);return}originalAnchorClick.call(this)}
 document.addEventListener('click',downloadClick,true)
 HTMLFormElement.prototype.submit=function(){
  const form=this
  // Download navigation stays in the owning bridge. In particular, the
  // original gallery's hidden ZIP form must never navigate the native shell.
  void(async()=>{const url=sameOwner(form.action),params=new URLSearchParams();for(const [key,value]of new FormData(form) as any){if(typeof value!=='string')throw Error('File forms require the upload API');params.append(key,value)}const method=form.method.toUpperCase();const response=await globalThis.fetch(method==='GET'?url+(url.includes('?')?'&':'?')+params.toString():url,method==='GET'?{}:{method,body:params});if(!response.ok)throw Error('Export failed ('+response.status+')');const disposition=response.headers.get('content-disposition')??'';const name=/filename="?([^";]+)"?/.exec(disposition)?.[1]??'export.zip';await save(await response.blob(),name)})().catch(downloadError)
 }
 class Socket extends EventTarget{
  static CONNECTING=0;static OPEN=1;static CLOSING=2;static CLOSED=3
  CONNECTING=0;OPEN=1;CLOSING=2;CLOSED=3;readyState=0;bufferedAmount=0;binaryType='blob';extensions='';protocol='';url:string;id?:string;timer?:number;onopen:any;onmessage:any;onclose:any;onerror:any;route:string
  constructor(input:string|URL){super();this.url=String(input);const url=new URL(this.url);if(!['ws:','wss:'].includes(url.protocol)||url.host!=='surface')throw Error('External WebSocket refused');this.route='ws:'+url.pathname;channels.add(this);void this.open(url.pathname+url.search)}
  private emit(name:string,event:Event){this.dispatchEvent(event);(this as any)['on'+name]?.(event)}
  private async open(url:string){try{const row=await request(this.route,{op:'open',url});this.id=row.id;if(this.readyState!==0){await request(this.route,{op:'close',id:this.id});return}this.readyState=1;this.emit('open',new Event('open'));this.poll()}catch{this.end(1006)}}
  private poll(){if(closed||this.readyState!==1)return;this.timer=window.setTimeout(async()=>{try{const value=await request(this.route,{op:'read',id:this.id});for(const event of value.events){if(event.type==='close'){this.end(event.code);return}const data=event.base64?Uint8Array.from(atob(event.base64),x=>x.charCodeAt(0)):event.text;this.emit('message',new MessageEvent('message',{data:typeof data==='string'?data:this.binaryType==='arraybuffer'?data.buffer:new Blob([data])}))}this.poll()}catch{this.end(1006)}},200)}
  send(data:any){if(this.readyState!==1)throw new DOMException('Socket not open','InvalidStateError');if(typeof data!=='string')throw Error('Binary socket send not supported');if(new TextEncoder().encode(data).length>65536)throw Error('Socket send exceeds limit');void request(this.route,{op:'send',id:this.id,text:data}).catch(()=>this.end(1006))}
  close(code=1000){if(this.readyState===3)return;this.readyState=2;clearTimeout(this.timer);if(this.id)void request(this.route,{op:'close',id:this.id}).then(()=>this.end(code),()=>this.end(1006));else this.end(code)}
  private end(code:number){if(this.readyState===3)return;this.readyState=3;channels.delete(this);clearTimeout(this.timer);if(code!==1000)this.emit('error',new Event('error'));this.emit('close',new CloseEvent('close',{code,wasClean:code===1000}))}
 }
 class Source extends EventTarget {
  static CONNECTING=0;static OPEN=1;static CLOSED=2;CONNECTING=0;OPEN=1;CLOSED=2;readyState=0;withCredentials=false;url:string;id?:string;timer?:number;buffer='';lastEventId='';onopen:any;onmessage:any;onerror:any
  constructor(input:string|URL){super();this.url=String(input);const url=sameOwner(input);channels.add(this);void this.open(url)}
  private emit(name:string,event:Event){this.dispatchEvent(event);(this as any)['on'+name]?.(event)}
  private async open(url:string){try{const value=await terminal(await request('http',{url,method:'GET'}));if(!value.eventHandle)throw Error('SSE response required');this.id=value.eventHandle;if(this.readyState===2){await request('http.events',{id:this.id,release:true});return}this.readyState=1;this.emit('open',new Event('open'));this.poll()}catch{this.fail()}}
  private poll(){if(closed||this.readyState!==1)return;this.timer=window.setTimeout(async()=>{try{const value=await request('http.events',{id:this.id});this.buffer+=value.chunks.join('').replace(/\r\n/g,'\n');if(this.buffer.length>262144)throw Error('SSE frame exceeds limit');let at;while((at=this.buffer.indexOf('\n\n'))>=0){const lines=this.buffer.slice(0,at).split('\n');this.buffer=this.buffer.slice(at+2);let type='message';const data=[];for(const line of lines){if(line.startsWith('event:'))type=line.slice(6).trim();else if(line.startsWith('data:'))data.push(line.slice(5).replace(/^ /,''));else if(line.startsWith('id:'))this.lastEventId=line.slice(3).trim()}if(data.length)this.emit(type,new MessageEvent(type,{data:data.join('\n'),lastEventId:this.lastEventId}))}if(value.closed){this.close();return}this.poll()}catch{this.fail()}},200)}
  close(){if(this.readyState===2)return;this.readyState=2;channels.delete(this);clearTimeout(this.timer);if(this.id)void request('http.events',{id:this.id,release:true}).catch(()=>{})}
  private fail(){if(this.readyState===2)return;this.emit('error',new Event('error'));this.close()}
 }
 globalThis.EventSource=Source as any
 globalThis.WebSocket=Socket as any
 ;(globalThis as any).__DSH_TRANSPORT__={streamBaseUrl:'http://surface/'}
 return ()=>{if(closed)return;closed=true;if(clipboardDescriptor)Object.defineProperty(navigator,'clipboard',clipboardDescriptor);else delete (navigator as any).clipboard;for(const url of blobs.keys())originalRevoke.call(URL,url);blobs.clear();blobBytes=0;URL.createObjectURL=originalCreate;URL.revokeObjectURL=originalRevoke;document.removeEventListener('click',downloadClick,true);HTMLFormElement.prototype.submit=originalSubmit;HTMLAnchorElement.prototype.click=originalAnchorClick;for(const id of pending)void request('http.pending',{id,release:true}).catch(()=>{});pending.clear();for(const channel of [...channels])channel.close();globalThis.fetch=originalFetch;globalThis.WebSocket=OriginalSocket;globalThis.EventSource=OriginalSource}
}
