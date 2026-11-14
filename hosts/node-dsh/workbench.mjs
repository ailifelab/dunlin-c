// Optional browser Remote faces. Native C selects the owner/version before any
// invocation. Only explicitly supported public methods cross this boundary.
import {Service} from '@deepseek-ai/cordis';
export function installWorkbenchServices(root,{owned,commands,contributions,metadata,native}){
 const sections=new Map();
 class Settings extends Service {
  constructor(ctx){super(ctx,'settings')}
  installSection(scope,namespace,schema,initial,hooks){
   if(typeof hooks?.setSource!=='function'||typeof schema!=='function')throw Error('Unsupported live settings section');
   const row={schema,value:initial,hooks};const key=owned(scope,'settings:'+String(namespace),row,sections);hooks.setSource(()=>row.value);scope.effect(()=>()=>sections.delete(key));return()=>sections.delete(key);
  }
  register(namespace,schema,options){
   if(namespace!=='checkpoint-rewind'||typeof schema!=='function'||typeof options?.validate!=='function')throw Object.assign(Error('Unsupported settings namespace'),{code:-32602});
   const parse=config=>{const next=schema(structuredClone(config));options.validate(next);return structuredClone(next)};
   const row={schema:parse,value:parse(options.base),watchers:new Set(),hooks:{onChange(){for(const watcher of row.watchers)watcher(structuredClone(row.value))}}};
   const key=owned(this.ctx,'settings:'+namespace,row,sections);this.ctx.effect(()=>()=>{sections.delete(key);row.watchers.clear()});
   return {get:()=>structuredClone(row.value),watch:listener=>{if(typeof listener!=='function')throw Error('Invalid settings watcher');row.watchers.add(listener);return()=>row.watchers.delete(listener)},async replace(config){
    if(!sections.has(key)||!native?.available)throw Object.assign(Error('Native settings unavailable'),{code:-32602});const next=parse(config);
    const before=native.call('settings.hash',{});if(!before?.ok)throw Object.assign(Error(before?.error??'Native settings unavailable'),{code:-32602});
    const committed=native.call('settings.commit',{namespace,expectedHash:before.data.hash,config:next});if(!committed?.ok)throw Object.assign(Error(committed?.error??'Native settings commit refused'),{code:-32602});
    row.value=next;row.hooks.onChange();const acknowledged=native.call('settings.applied',{namespace,hash:committed.data.hash});if(!acknowledged?.ok)throw Object.assign(Error(acknowledged?.error??'Native settings acknowledgement refused'),{code:-32602});return structuredClone(row.value);
   }};
  }
  describe(){return []}
 }
 new Settings(root);
 return {settings(plugin,config,apply=false){
  const rows=[...sections.values()].filter(row=>row.owner===plugin).map(row=>row.value);
  if(!rows.length)return {supported:false};
  let parsed;try{parsed=rows.map(row=>row.schema(config))}catch{throw Object.assign(Error('Invalid live plugin configuration'),{code:-32602})}
  if(apply){for(let i=0;i<rows.length;i++)rows[i].value=structuredClone(rows[i].watchers?parsed[i]:config);for(const row of rows)row.hooks.onChange?.()}
  return {supported:true};
 },marketProjection(args,response,rows,managed){
  if(!Array.isArray(rows)||!response?.body||response.status!==200)return response;
  const url=new URL(args.url,'http://127.0.0.1');
  // Pinned upstream's official Desktop runtime also reports manager
  // availability through pnpm; this is the C manager, not a pnpm install.
  if(url.pathname==='/dsh-market/status'&&managed?.available===true&&managed.mode==='native')return {...response,body:{...response.body,pnpm:true,managedInstall:managed}};
  if(url.pathname==='/dsh-market/installed'){
   const present=rows.map(row=>({...row,...(row.versions?.find(v=>v.version===row.version)??{})})).filter(r=>r.manifest?.engines?.host==='node-dsh');const installed={},activation={};
   for(const row of present){const name=row.package;installed[name]=row.manifest.upstreamVersion??row.version;activation[name]={state:row.quarantined?'failed':row.active?'live':'disabled',reasons:row.reason?[row.reason]:[],bundle:false,hot:row.active===true}}
   return {...response,body:{...response.body,profile:'Dunlin C',installed,present:present.map(r=>r.package),activation,live:present.filter(r=>r.active).map(r=>r.package),disabled:present.filter(r=>!r.active).map(r=>r.package),patchDisabled:[],patchForced:[],bundles:[]}};
  }
  return response;
 },workspaces(){return root.workspaceRegistry.list().map(w=>({workspaceId:w.id,name:w.title,title:w.title,path:w.path,cwd:w.path,sessionIds:[...w.sessionIds]}))},workspace(id){const w=root.workspaceRegistry.get(id);return w?{workspaceId:w.id,title:w.title,path:w.path}:null},async remote(plugin,args){
  const name=metadata.get(plugin)?.name;const {namespace,method}=args;const values=args.args??[];
  if(!Array.isArray(values)||values.length>8)throw Object.assign(Error('Invalid Remote arguments'),{code:-32602});
  if(name==='dsh-checkpoint-rewind'&&namespace==='checkpointPanel'&&['timeline','diff','restorePreview'].includes(method)){
   const api=root.get('checkpointPanel');if(!api)throw Object.assign(Error('Remote service unavailable'),{code:-32602});
   if(method==='timeline'&&(values[0]!==null&&values[0]!==undefined)&&(!Number.isInteger(values[0])||values[0]<1||values[0]>200))return {ok:false,error:{code:'invalid_input',message:'Limit must be 1..200'}};
   if(method!=='timeline'&&values.some(v=>typeof v!=='string'||v.length>256))return {ok:false,error:{code:'invalid_input',message:'Invalid checkpoint reference'}};
   const value=await api[method](...values.map(x=>x===null?undefined:x));return {ok:true,value};
  }
  if(name==='dsh-image-gen'&&namespace==='credentials'&&['describe','set','unset'].includes(method)){
   const api=root.get('credentials');const refs=method==='describe'?values[0]:[values[0]];
   if(!Array.isArray(refs)||refs.length>32||refs.some(v=>typeof v!=='string'||!/^[A-Za-z][.:/A-Za-z0-9_-]{1,160}$/.test(v)))return {ok:false,error:{code:'invalid_input',message:'Invalid image credential reference'}};
   if(method==='describe')return {ok:true,value:Object.fromEntries(await Promise.all(refs.map(async ref=>[ref,await api.describe(ref)])))};
   if(method==='set'&&(typeof values[1]!=='string'||!values[1]||Buffer.byteLength(values[1])>8192))return {ok:false,error:{code:'invalid_input',message:'Credential must be 1..8192 bytes'}};
   await api[method](...values);return {ok:true};
  }
  throw Object.assign(Error('Remote method unavailable for this owner'),{code:-32602});
 }};
}
