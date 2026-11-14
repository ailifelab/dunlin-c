// Native-owned DSH workbench composition. No model loop or hidden execution.
import * as React from 'react'
import {installTerminals} from './terminals'
import {installJobs} from './jobs'
export function observation<T>(initial:T){let value=initial;const listeners=new Set<()=>void>();return {getSnapshot:()=>value,subscribe:(fn:()=>void)=>{listeners.add(fn);return()=>listeners.delete(fn)},set(next:T){value=next;for(const fn of listeners)fn()}}}
export const workbenchSlots:any={
 'main':{kind:'keyed',scope:'root'},'sidebar.panellist':{kind:'list',scope:'root'},
 'settings.section':{kind:'list',scope:'root'},'settings.section.icon':{kind:'list',scope:'root'},
 'settings.plugin.item':{kind:'keyed',scope:'root'},'settings.plugins.tab':{kind:'list',scope:'root'},
 'plugins.tab':{kind:'list',scope:'root'},'plugins.bundle.config':{kind:'keyed',scope:'root'},'plugins.detail.section':{kind:'list',scope:'root'},
 'shell.overlay':{kind:'list',scope:'root'},
 'conversation.view':{kind:'list',scope:'session'},'conversation.chat.node':{kind:'keyed',scope:'session'},
 'conversation.input.right':{kind:'list',scope:'session'},'conversation.input.dock':{kind:'list',scope:'session'},
 'conversation.session.header.utilities':{kind:'list',scope:'session'},'tool.call.toolview':{kind:'keyed',scope:'session'},
 'sidebar.right.pane.tab':{kind:'keyed',scope:'session'},'sidebar.right.pane.tab.title':{kind:'keyed',scope:'session'},
}
export function installWorkbench(root:any,sessionID:string,options:any,table:Map<string,unknown>){
 const request=options.request??(async()=>{throw Error('Native owner transport unavailable')})
 installTerminals(root,request)
 const jobs=installJobs(root,request,observation,sessionID)
 const empty={sessions:{ids:[],byId:{},phase:'ready'},workspaces:{items:[],archivedSessionIds:[]},current:sessionID}
 const native=observation<any>(options.native??empty),panel=observation<any>({activePanelId:null}),tabs=observation<any[]>([]),selectedTab=observation<string|undefined>(undefined),connectionState=observation('connected'),rightExpanded=observation(false),wide=observation(false)
 const types=new Map<string,any>(),closeHandlers=new Map<string,any>(),scopes=new Map<string,any>()
 const sessionList={getSnapshot:()=>native.getSnapshot().sessions??empty.sessions,subscribe:native.subscribe}
 const workspaceList={getSnapshot:()=>native.getSnapshot().workspaces??empty.workspaces,subscribe:native.subscribe}
 const current=observation(sessionID)
 if(options.package==='dshmarket'){
  const policy=options.native?.managedInstall
  const allowed=new Set<string>(policy?.available===true&&policy.mode==='native'&&Array.isArray(policy.repositories)?policy.repositories.filter((url:any)=>typeof url==='string'):[])
  const manager=Object.freeze({ready:policy?.available===true&&policy.mode==='native',allows:(url:string)=>allowed.has(url)})
  ;(globalThis as any).__DUNLIN_MANAGED_INSTALL__=manager
  root.effect(()=>()=>{if((globalThis as any).__DUNLIN_MANAGED_INSTALL__===manager)delete (globalThis as any).__DUNLIN_MANAGED_INSTALL__})
 }
 let alive=true,polling=false;const refresh=async()=>{if(!alive||polling)return;polling=true;try{const value=await request('native.snapshot',{});if(alive){if(connectionState.getSnapshot()!=='connected')connectionState.set('connected');if(JSON.stringify(value)!==JSON.stringify(native.getSnapshot()))native.set(value)}}catch(error){if(alive){if(connectionState.getSnapshot()!=='disconnected')connectionState.set('disconnected');options.onError?.(error)}}finally{polling=false}};const timer=window.setInterval(()=>void refresh(),1500);root.effect(()=>()=>{alive=false;clearInterval(timer)})
 async function action(method:string,args:any={}){const result=await request('native.'+method,args);if(result?.snapshot)native.set(result.snapshot);return result?.value??result}
 const open=(id:string)=>{void action('session.open',{id}).catch(options.onError??console.error)}
 function scope(id:string){
  if(!scopes.has(id)){
   const ctx=root.extend().isolate('session').isolate('input').isolate('inputActions');const data=observation<any>({id,messages:[],phase:'idle',status:'idle',input:{text:'',occurrences:[]}})
   const input=observation<any>({text:'',occurrences:[]})
   const inputActions={setText:(text:string)=>{input.set({...input.getSnapshot(),text});return action('draft.set',{id,text})},insertText:(text:string)=>inputActions.setText(input.getSnapshot().text+text)}
   const session=data;session.set({...session.getSnapshot(),...(sessionList.getSnapshot().byId[id]??{})});const unsubscribe=native.subscribe(()=>session.set({...session.getSnapshot(),...(sessionList.getSnapshot().byId[id]??{})}));root.effect(()=>unsubscribe)
   ctx.provide('session',session);ctx.provide('input',input);ctx.provide('inputActions',inputActions)
   const binding={key:id,ctx,hooks:{useSession:session,useInput:input},keyedHooks:{},props:{sessionId:id,inputActions}};scopes.set(id,{ctx,session,input,inputActions,binding})
  }
  return scopes.get(id)
 }
 const using=async(id:string,_options:any,run:(ref:any)=>unknown)=>{
  if(!alive||!sessionList.getSnapshot().byId[id])return {ok:false,error:{code:'session_unavailable',message:'会话不可用'}}
  const prompt=(content:any,mode:string)=>action('session.prompt',{id,content,mode})
  try{return {ok:true,value:await run({binding:{session:{prompt}},ctx:scope(id).ctx})}}
  catch(error:any){return {ok:false,error:{code:error.code??'native_prompt_refused',message:String(error.message??error)}}}
 }
 const sessions={list:sessionList,using,scope:(id:string)=>scope(id).ctx,binding:(id:string)=>scope(id).binding,open,create:async(args:any)=>{const result=await action('session.create',args);return result.id??result},fork:async(args:any)=>{const result=await action('session.fork',args);return result.id??result}}
 root.provide('sessions',sessions);root.provide('workspaces',{list:workspaceList,create:(args:any)=>action('workspace.create',args),update:(args:any)=>action('workspace.update',args),archiveSession:(id:string,_options:any)=>action('session.archive',{id}),unarchiveSession:(id:string)=>action('session.unarchive',{id})})
 root.provide('uiWorkspace',{openSession:open,pickDirectory:()=>action('directory.pick'),createSession:(args:any)=>sessions.create(args)})
 root.provide('layout',{panelInfo:panel,selectPanel:(id:string|null)=>panel.set({...panel.getSnapshot(),activePanelId:id})})
 const statuses=observation(new Map(Object.entries(sessionList.getSnapshot().byId).map(([id,row]:any)=>[id,{phase:row.phase??'idle'}])));root.effect(()=>native.subscribe(()=>statuses.set(new Map(Object.entries(sessionList.getSnapshot().byId).map(([id,row]:any)=>[id,{phase:row.phase??'idle'}])))));
 const currentBinding=observation(scope(sessionID).binding);root.provide('uiSession',{adapter:{current:currentBinding},sessionStatus:statuses})
 root.provide('modules',{import:async(name:string)=>{if(!table.has(name))throw Error('Unsupported client module');return table.get(name)}})
 root.provide('connection',{url:location.origin,baseURL:location.href,api:{},state:connectionState})
 const remote:any={$mount:async(_descriptors:any)=>{},$on:(_name:string,_fn:any)=>()=>{}}
 const remoteCall=(namespace:string,method:string,args:any[])=>request('rpc',{namespace,method,args})
 const remoteMethods:any={checkpointPanel:['timeline','diff','restorePreview'],credentials:['describe','set','unset'],session:['openWorkspacePath','canOpenWorkspacePath','read','list','fork','create','rename','delete']}
 for(const [namespace,methods]of Object.entries(remoteMethods) as [string,string[]][]){remote[namespace]=Object.fromEntries(methods.map(method=>[method,(...args:any[])=>namespace==='session'?request('native.remoteSession',{method,args}):remoteCall(namespace,method,args)]));root.provide('remote.'+namespace,remote[namespace])}
 remote.commands={execute:(chat:string,line:string,attachments:any[])=>{
  const match=/^\/([\w-]+)(?:\s+(.*))?$/.exec(line)
  if(chat!==sessionID||!match||attachments.length)throw Error('Unsupported command selection')
  return request('slash:'+match[1],{input:match[2]??''})
 }}
 root.provide('remote.commands',remote.commands)
 const decorations=new Map<string,any>()
 root.provide('commandUi',{decorate(item:any){decorations.set(item.name,item);return ()=>decorations.delete(item.name)},async candidates(){return Array.from(decorations.values()).filter(item=>item.available?.()!==false)}})
 root.provide('remote',remote)
 const config=observation<any>({value:options.config??{},writable:true,status:'ready'})
 const configScope={...config,async set(field:string,value:any){const next=await request('config.set',{field,value});config.set({value:next,writable:true,status:'ready'});return true}}
 root.provide('settingsScope',{bind:()=>configScope});root.provide('configForms',{get:()=>configScope})
 root.provide('pluginNavigation',{openBundle:(name:string)=>action('plugin.open',{name})})
 root.provide('sidebarRightTabs',{register(def:any){if(!def||typeof def.id!=='string'||types.has(def.id))throw Error('Invalid sidebar tab type');types.set(def.id,def);return()=>{types.delete(def.id);tabs.set(tabs.getSnapshot().filter(t=>t.implementation!==def.id))}}})
 function openTab(kind:string,opts:any={}){
  const type=[...types.values()].find(t=>t.kind===kind);if(!type)throw Error('Sidebar tab type unavailable: '+kind)
  const existing=tabs.getSnapshot().find(t=>t.kind===kind&&(type.multiple!==true||t.contentId===(opts.contentId??kind)))
  if(existing&&!opts.preferNewPane){tabs.set(tabs.getSnapshot().map(t=>t.id===existing.id?{...t,navigation:{...t.navigation,revision:t.navigation.revision+1,params:{...t.navigation.params,...opts.params}}}:t));if(opts.revealIfOpened!==false){selectedTab.set(existing.id);rightExpanded.set(true)}return existing.id}
  const tab={id:crypto.randomUUID(),kind,contentId:opts.contentId??kind,implementation:type.id,navigation:{address:opts.contentId??'',revision:0,params:opts.params??{}}};tabs.set([...tabs.getSnapshot(),tab]);if(opts.revealIfOpened!==false||!selectedTab.getSnapshot()){selectedTab.set(tab.id);rightExpanded.set(true)}return tab.id
 }
 const sidebarRight:any={mounted:current,isExpanded:()=>rightExpanded.getSnapshot()&&tabs.getSnapshot().length>0,active:()=>tabs.getSnapshot().find(t=>t.id===selectedTab.getSnapshot()),toggleExpanded:()=>rightExpanded.set(!rightExpanded.getSnapshot()),openTab,openTabIn:(_id:string,kind:string,opts:any)=>openTab(kind,opts),openResource:(address:string,opts:any={})=>{const type=[...types.values()].find(t=>t.canOpen?.(address));if(!type)throw Error('No resource viewer');return openTab(type.kind,{...opts,contentId:address})},close:(id:string)=>{const row=tabs.getSnapshot().find(t=>t.id===id);if(row)closeHandlers.get(row.kind)?.(sessionID,row);tabs.set(tabs.getSnapshot().filter(t=>t.id!==id));selectedTab.set(tabs.getSnapshot().at(-1)?.id)},focus:(id:string)=>selectedTab.set(id),registerCloseHandler:(kind:string,fn:any)=>{closeHandlers.set(kind,fn);return()=>closeHandlers.delete(kind)}}
 sidebarRight.openResourceIn=(_id:string,address:string,opts:any)=>sidebarRight.openResource(address,opts);sidebarRight.closeIn=(_id:string,id:string)=>sidebarRight.close(id)
 root.provide('sidebarRight',sidebarRight)
 root.slots.provideRoot({hooks:{useSessions:sessionList,useWorkspaces:workspaceList},keyedHooks:{},props:{}})
 function hook(observable:any,selector:any=(v:any)=>v){const snap=React.useSyncExternalStore(observable.subscribe,observable.getSnapshot,observable.getSnapshot);return selector(snap)}
 function standard(){const row=scope(sessionID);return {sessionId:sessionID,useSession:(select:any)=>hook(row.session,select),useInput:(select:any)=>hook(row.input,select),inputActions:row.inputActions,useSessions:(select:any)=>hook(sessionList,select),useWorkspaces:(select:any)=>hook(workspaceList,select)}}
 const scopeAdapter={current:{getSnapshot:()=>sessions.binding(sessionID),subscribe:()=>()=>{}},renderArea:(_binding:any,props:any)=>props.children,bindingSource:(target:any)=>({getSnapshot:()=>sessions.binding(typeof target==='string'?target:sessionID),subscribe:()=>()=>{}})}
 if(options.package==='dsh-project-manager'){
  const normalize=()=>{const walker=document.createTreeWalker(document.body,NodeFilter.SHOW_TEXT);let node;while((node=walker.nextNode()))if(node.parentElement?.closest('.d-f .note .chip.warn,.dpm-dialog .ns-row .sub,.dpm-toasts .toast > span')&&node.nodeValue?.endsWith('完全权限'))node.nodeValue=node.nodeValue.replaceAll('完全权限','跟随聊天授权')};const observer=new MutationObserver(normalize);observer.observe(document.body,{childList:true,subtree:true});root.effect(()=>()=>observer.disconnect())
 }
 function Shell(props:any){
  const toolbar=React.useRef<HTMLElement>(null);React.useLayoutEffect(()=>{
   if(options.package!=='dsh-project-manager'||!toolbar.current)return
   const update=()=>document.documentElement.style.setProperty('--dunlin-workbench-content-top',Math.max(0,toolbar.current!.getBoundingClientRect().bottom)+'px')
   const observer=new ResizeObserver(update);observer.observe(toolbar.current);window.addEventListener('resize',update);update()
   return()=>{observer.disconnect();window.removeEventListener('resize',update);document.documentElement.style.removeProperty('--dunlin-workbench-content-top')}
  },[])
  const [,pulse]=React.useState(0);React.useEffect(()=>{const stops=Object.keys(workbenchSlots).map(k=>root.slots.subscribe(k,()=>pulse(n=>n+1)));return()=>stops.forEach(fn=>fn())},[]);const entries=(key:string)=>root.slots.entries(key).map((x:any)=>x.options);
  const jobErrors=Object.values(hook(jobs.state).errors);const [page,setPage]=React.useState('主工作台');const active=hook(panel).activePanelId;const openTabs=hook(tabs),selected=hook(selectedTab),expanded=hook(rightExpanded),full=hook(wide)&&expanded&&openTabs.length>0;const liveTab=openTabs.find((t:any)=>t.id===selected)
  const keyProps={...standard(),view:'config',subject:{pkg:{name:options.package}},surface:'settings'}
  const pages=['主工作台','设置','对话扩展']
  return <section id="root" className="dunlin_frame" data-dunlin-workbench={options.package} style={{height:'100%',minHeight:620,display:'flex',flexDirection:'column',overflow:'hidden'}}>
   {options.package==='dsh-project-manager'?<style>{'body > .dpm-global-detail{top:var(--dunlin-workbench-content-top,0px);height:auto}'}</style>:null}
   <nav ref={toolbar} aria-label="工作台分区" style={{display:'flex',gap:8,padding:'8px 12px',borderBottom:'1px solid var(--dsw-alias-border-l2,#ddd)'}}>
    {pages.map(title=><button key={title} onClick={()=>{setPage(title);wide.set(false);if(title!=='主工作台')rightExpanded.set(false)}} aria-pressed={page===title}>{title}</button>)}<span style={{flex:1}}/><span onClickCapture={()=>wide.set(false)}>{props.renderSlot('conversation.session.header.utilities',keyProps)}</span>
    {[...types.values()].filter((type:any)=>type.guide?.length).map((type:any)=><button key={type.id} onClick={()=>{setPage('主工作台');openTab(type.kind);wide.set(options.package==='dsh-image-gen')}}>{type.guide[0].title()}</button>)}
    {entries('sidebar.panellist').map((e:any)=><button key={e.id??e.key} onClick={()=>{panel.set({activePanelId:e.id??e.key});setPage('主工作台')}}>{typeof e.label==='function'?e.label():e.label??e.id??e.key}</button>)}
   </nav>
   {options.package==='dshmarket'?<div role="note" style={{padding:'6px 12px',fontSize:12,color:'var(--dsw-alias-label-secondary)'}}>插件由 Dunlin C 托管安装。未验证的版本请谨慎安装；更新和启用仍须核对版本及权限。</div>:null}
   {jobErrors.length?<div role="alert" style={{padding:8}}>任务同步失败：{String(jobErrors[0])}<button onClick={()=>jobs.refresh()}>重试</button></div>:null}
   <div className="dunlin_workbenchFrame" style={{position:'relative',display:'grid',gridTemplateColumns:full?'0px minmax(0,1fr)':`minmax(0,1fr) ${openTabs.length&&expanded?'min(45%,520px)':'0px'}`,flex:1,minHeight:0,overflow:'hidden'}}>
    <main hidden={Boolean(full)} className="dunlin_centerCol" style={{gridColumn:1,gridRow:1,minWidth:0,overflow:'auto',position:'relative'}}>
     {page==='设置'?<>{props.renderSlot('settings.section',keyProps)}{props.renderSlot('settings.plugins.tab',keyProps)}{props.renderSlot('plugins.bundle.config',keyProps,{entryKey:options.package})}{props.renderSlot('settings.plugin.item',keyProps,{entryKey:options.package})}</>:
      page==='对话扩展'?<>{props.renderSlot('conversation.input.dock',keyProps)}{props.renderSlot('conversation.input.right',keyProps)}{props.renderSlot('conversation.composer.dock',keyProps)}</>:
      <>{active?props.renderSlot('main',keyProps,{entryKey:active}):null}{props.renderSlot('plugins.tab',keyProps)}{!entries('main').length?props.renderSlot('settings.plugins.tab',keyProps):null}<div data-slot="main.conversation" style={{display:'contents'}}>{props.renderSlot('conversation.view',keyProps)}</div>{!active&&entries('main').length?props.renderSlot('main',keyProps,{entryKey:entries('main')[0].key}):null}</>}
    </main>
    <aside className="dunlin_rightbarCol" data-rightbar-col="" data-sidebar-right-open={Boolean(openTabs.length&&expanded)} style={{gridColumn:2,gridRow:1,position:'relative',minWidth:0}}>
     {openTabs.length?<div hidden={!expanded} data-sidebar-right-panel="" style={{position:'absolute',inset:'0 0 0 auto',width:'100%',overflow:'auto',background:'var(--dsw-alias-bg-layer-1,#fff)',borderLeft:'1px solid var(--dsw-alias-border-l2,#ddd)'}}>
      <nav aria-label="侧栏标签" style={{display:'flex',gap:4,padding:8,overflow:'auto'}}>{openTabs.map((t:any)=><button key={t.id} onClick={()=>selectedTab.set(t.id)} aria-pressed={selected===t.id}>{t.navigation.params.title??types.get(t.implementation)?.guide?.[0]?.title?.()??t.kind}</button>)}{selected?<button onClick={()=>wide.set(!wide.getSnapshot())}>{full?'收起工作区':'展开工作区'}</button>:null}{selected?<button aria-label="关闭当前标签" onClick={()=>sidebarRight.close(selected)}>×</button>:null}</nav>
      {openTabs.map((tab:any)=>{const visible=expanded&&selected===tab.id,view={...tab,visible};return visible||types.get(tab.implementation)?.keepMounted?<div key={tab.id} hidden={!visible} style={{height:'calc(100% - 48px)',minHeight:0}}>{props.renderSlot('sidebar.right.pane.tab',{...keyProps,tab:view,navigation:tab.navigation,useTabInfo:(select:any)=>{const value={tab:view,navigation:tab.navigation};return select?select(value):value},tabId:tab.id},{entryKey:tab.implementation})}</div>:null})}
     </div>:null}
    </aside>
   </div>
   {props.renderSlot('shell.overlay',keyProps)}
  </section>
 }
 return {native,scopeAdapter,standard,Shell,types,sessions,setConfiguration(value:any){if(!value||typeof value!=='object'||Array.isArray(value)||new TextEncoder().encode(JSON.stringify(value)).length>32768)throw Error('Invalid host configuration');config.set({value:structuredClone(value),writable:true,status:'ready'})}}
}
