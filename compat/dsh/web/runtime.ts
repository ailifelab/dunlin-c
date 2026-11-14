// Optional DSH browser compatibility. No filesystem, credentials, model/tool
// dispatch or persistence live here; the C/native owner supplies approved modules.
import * as React from 'react'
import * as Zod from 'zod'
import * as JSXRuntime from 'react/jsx-runtime'
import * as Store from '@deepseek-ai/dsh-client-store'
import * as Primitives from '@deepseek-ai/dsh-client-ui-primitives'
import ThemeRegistration from 'dunlin-client:@deepseek-ai/dsh-client-ui-theme'
import LocaleRegistration from 'dunlin-client:@deepseek-ai/dsh-client-locale'
import * as ReactDOM from 'react-dom'
import * as ReactDOMClient from 'react-dom/client'
import * as Cordis from '../vendor/cordis/src/index.ts'
import * as Slots from './vendor/ui-slots/src/index.ts'
import * as Renderer from './vendor/ui-renderer/src/client/index.ts'
import {installWorkbench,workbenchSlots,observation} from './workbench.tsx'
import {installPresentation} from './presentation.ts'

export const supportedSlots = Object.freeze(['conversation.composer.dock','conversation.chat.reasoning.body','settings.general.item','dunlin.workbench','shell.overlay'])
const baseline = new Map<string,unknown>([
  ['@deepseek-ai/dsh-client-runtime/client',Object.freeze({defineStore:Store.defineStore})],
  ['zod',Zod],['react',React],['react/jsx-runtime',JSXRuntime],['@deepseek-ai/dsh-client-store',Store],['@deepseek-ai/dsh-client-ui-primitives',Primitives],['react-dom',ReactDOM],['react-dom/client',ReactDOMClient],
  ['cordis',Cordis],['@deepseek-ai/cordis',Cordis],['@deepseek-ai/dsh-client-ui-slots',Slots],
])
type Registration = {id:string,factory:(require:(id:string)=>unknown)=>any}
type Fault = {plugin:string,phase:string,code:string}
export async function createSurface(sessionID:string,report:(fault:Fault)=>void = ()=>{},options:any={}) {
  if(!sessionID || sessionID.length>128)throw Error('invalid surface session')
  const root:any=new Cordis.Context()
  const plugins=new Map<string,any>()
  let ready=false;let disposed=false,closing=false,tail:Promise<unknown>=Promise.resolve(),unmount:(()=>void)|undefined
  await root.plugin(Renderer)
  const requireSDK=(name:string)=>{if(!baseline.has(name))throw Error('unsupported SDK import: '+name);return baseline.get(name)};
  const {LocaleRuntime}=LocaleRegistration.factory(requireSDK);
  root.provide('locale',new LocaleRuntime(root,undefined,{languages:['zh-CN'],preference:'zh'}));root.slots.installLocale(root.locale);
  const {ThemeRuntime}=ThemeRegistration.factory(requireSDK);
  const themeListeners=new Set<()=>void>();let themeSettings:any={fontSize:14,codeFontSize:11,terminalFontSize:13,textFontFamily:'',codeFontFamily:'',terminalFontFamily:'',preference:['light','dark','system'].includes(options.theme)?options.theme:'system'};
  const host={getSnapshot:()=>({value:themeSettings}),subscribe:(fn:()=>void)=>{themeListeners.add(fn);return()=>themeListeners.delete(fn)},set:async(key:string,value:any)=>{themeSettings={...themeSettings,[key]:value};for(const fn of themeListeners)fn()}};
  root.provide('theme',new ThemeRuntime(root,host));
  const appliedTokens=new Set<string>();let removeSelectedTheme:(()=>void)|undefined,hostPreference:{id:string,owner?:string}|undefined;let applyingTheme=0;
  const presentation=installPresentation(root,options,()=>hostPreference);
  const setThemeDefinition=(definition:any,fallback='system')=>{
    if(definition&&(typeof definition.id!=='string'||definition.id.length>200||!['light','dark'].includes(definition.colorScheme)||!definition.tokens||Object.keys(definition.tokens).length>256||Object.entries(definition.tokens).some(([key,value])=>!/^--(?:dsw|shiki)-[A-Za-z0-9-]{1,110}$/.test(key)||typeof value!=='string'||value.length>256||!/^[A-Za-z0-9#(),.% -]+$/.test(value)||/url/i.test(value))))throw Error('Invalid selected theme');
    applyingTheme++;hostPreference=definition?{id:definition.id,owner:definition.owner}:undefined;
    try {removeSelectedTheme?.();removeSelectedTheme=undefined;
    if(definition){const selected={...definition,id:'dunlin-selected:'+definition.id};removeSelectedTheme=root.theme.register(selected);root.theme.setTheme(selected.id)}else root.theme.setTheme(fallback)
    }finally{applyingTheme--;presentation.apply()}
  };
  root.effect(()=>()=>removeSelectedTheme?.());
  root.on('theme/change',(snapshot:any)=>{
    if(typeof document==='undefined')return;
    if(snapshot.active?.colorScheme==='dark')document.body.dataset.dsDarkTheme='true';else delete document.body.dataset.dsDarkTheme;
    const tokens=snapshot.active?.tokens??{};for(const key of appliedTokens)if(!(key in tokens)){document.body.style.removeProperty(key);appliedTokens.delete(key)}
    for(const [key,value] of Object.entries(tokens))if((key.startsWith('--dsw-')||key.startsWith('--shiki-'))&&typeof value==='string'){document.body.style.setProperty(key,value);appliedTokens.add(key)}
    if(ready&&!applyingTheme)options.onTheme?.(snapshot.preference);
  });
  const selectedSlot=supportedSlots.includes(options.slot)?options.slot:'conversation.composer.dock';
  const workbench=(selectedSlot==='dunlin.workbench'||selectedSlot==='shell.overlay')?installWorkbench(root,sessionID,options,baseline):undefined;
  const reasoning=selectedSlot==='conversation.chat.reasoning.body'
  let updateReasoningConfig: ((value:any)=>void)|undefined
  if(reasoning){
    const invoke=async(method:string,input:any,signal?:AbortSignal)=>{
      signal?.throwIfAborted()
      const result=await options.request('reasoning.service',{method,...(method==='translate'?{request:input}:{})})
      signal?.throwIfAborted()
      return result
    }
    const service={
      limits:(signal?:AbortSignal)=>invoke('limits',undefined,signal),
      translate:(input:any,signal?:AbortSignal)=>invoke('translate',{...input,sessionId:sessionID},signal),
    }
    root.provide('remote.cotTranslation',service)
    // This surface exposes one host-bound service. Arbitrary Remote descriptors
    // cannot register new native endpoints.
    const listeners=new Map<string,Set<()=>void>>()
    const configuration=observation({value:options.config??{},writable:true,status:'ready'})
    updateReasoningConfig=value=>{
      configuration.set({value,writable:true,status:'ready'})
      listeners.get('settings/document-updated')?.forEach(notify=>notify())
    }
    const form={...configuration,async set(field:string,value:any){
      const next=await options.request('config.set',{field,value});updateReasoningConfig?.(next);return true
    }}
    root.provide('configForms',{get:()=>form})
    root.provide('remote',{cotTranslation:service,
      $on:(event:string,listener:()=>void)=>{
        if(!listeners.has(event))listeners.set(event,new Set())
        listeners.get(event)!.add(listener)
        return ()=>listeners.get(event)?.delete(listener)
      },
      $mount:async(contribution:any)=>{
        const descriptors=contribution?.descriptors
        if(contribution?.package!==options.package || !Array.isArray(descriptors) || !descriptors.length ||
          descriptors.some((d:any)=>d.namespace!=='cotTranslation' || !['limits','translate'].includes(d.method)))
          throw Error('Unsupported translation Remote descriptor')
        let mounted=true
        return async()=>{if(mounted){mounted=false;listeners.clear()}}
      },
    })
  }
  const binding=Object.freeze({key:sessionID,ctx:root,hooks:{},keyedHooks:{},props:{}})
  const absent=Object.freeze({key:undefined,hooks:{},keyedHooks:{},props:{}})
  const observable=(value:any)=>({getSnapshot:()=>value,subscribe:()=>()=>{}})
  await root.plugin({name:'dunlin-surface',inject:['slots'],apply(ctx:any){
    ctx.slots.installScope('session',{
      current:workbench?.scopeAdapter.current??observable(binding),
      renderArea:(current:any,props:any)=>current.key===sessionID ? props.children:(props.empty?.() ?? null),
      bindingSource:workbench?.scopeAdapter.bindingSource??((target:any)=>observable(target===sessionID || target?.id===sessionID ? binding:absent)),
    })
    if(reasoning)ctx.slots.registerFactory({name:'conversation.chat.reasoning.content'},(props:any)=>
      React.createElement('div',{style:{whiteSpace:'pre-wrap'}},props.text))
    ctx.slots.register({name:'root',id:'dunlin-surface',children:{
      'shell.overlay':{kind:'list',scope:'root'},'conversation.composer.dock':{kind:'list',scope:'session'},'settings.general.item':{kind:'list',scope:'root'},...(reasoning?{'conversation.chat.reasoning.body':{kind:'single',scope:'session'}}:{}),...(workbench?workbenchSlots:{}),
    }},(props:any)=>React.createElement(props.SessionProvider,null,
      React.createElement('div',{'data-dunlin-surface':sessionID},workbench&&selectedSlot==='dunlin.workbench'?React.createElement(workbench.Shell,props):props.renderSlot(selectedSlot,reasoning?{text:options.reasoningText??'',running:false}:{}))))
  }})
  root.slots.onEntryError((_key:string,entry:any)=>{
    const id=entry.registrant
    if(typeof id==='string' && plugins.has(id)){
      report({plugin:id,phase:'render',code:'plugin_render_failed'})
      queueMicrotask(()=>{void serialize(()=>remove(id)).catch(()=>{})})
    }
  })
  function serialize<T>(action:()=>Promise<T>):Promise<T>{
    const result=tail.then(action);tail=result.catch(()=>{});return result
  }
  async function remove(id:string){
    const fiber=plugins.get(id);if(!fiber)return
    plugins.delete(id);await fiber.dispose()
  }
  async function load(registration:Registration,expectedID:string,config:Record<string,unknown>={}){
    if(disposed)throw Error('surface disposed')
    if(!registration || registration.id!==expectedID || !expectedID || expectedID.length>256 || plugins.has(expectedID) || typeof registration.factory!=='function')throw Error('invalid client module')
    const require=(name:string)=>{
      if(!baseline.has(name))throw Error('unsupported client import')
      return baseline.get(name)
    }
    let fiber:any
    try {
      const exported=registration.factory(require),module=exported?.default ?? exported
      if(!module || typeof module.apply!=='function')throw Error('invalid client plugin')
      // Missing services must not silently wait in Cordis's pending state.
      const inject=module.inject ?? []
      const names=Object.keys(Cordis.Inject.resolve(inject))
      if(names.some((name:string)=>!root.get(name)))throw Error('unsupported client service')
      fiber=await root.plugin({...module,name:expectedID},config)
      if(!fiber || fiber.state!==Cordis.FiberState.ACTIVE || root.slots.entries('root').length!==1)throw Error('client activation failed')
      plugins.set(expectedID,fiber);if(options.themeDefinition)setThemeDefinition(options.themeDefinition,options.theme);else if(options.theme)root.theme.setTheme(options.theme);ready=true
    }catch(error){
      if(fiber)await fiber.dispose()
      report({plugin:expectedID,phase:'activate',code:'client_activation_failed'})
      throw error
    }
  }
  return {
    load:(registration:Registration,id:string,config:Record<string,unknown>={})=>closing?Promise.reject(Error('surface disposed')):serialize(()=>load(registration,id,config)),
    setTheme:(id:string)=>{if(disposed)throw Error('surface disposed');root.theme.setTheme(id)},
    setThemeDefinition:(definition:any,fallback:string)=>{if(disposed)throw Error('surface disposed');setThemeDefinition(definition,fallback)},
    setConfiguration:(value:any)=>{if(disposed||closing)throw Error('Surface disposed');if(workbench)workbench.setConfiguration(value);else if(updateReasoningConfig)updateReasoningConfig(value);else throw Error('Configuration unavailable')},
    setPresentations:(values:any)=>{if(disposed)throw Error('surface disposed');presentation.set(values)},
    remove:(id:string)=>serialize(()=>remove(id)),
    render:()=>{if(disposed)throw Error('surface disposed');return root.slots.renderSlot('root',{})},
    entries:(slot:string)=>disposed?[]:root.slots.entries(slot).map((x:any)=>({id:x.options.id,registrant:x.registrant})),
    mount(container:HTMLElement){if(disposed||unmount)throw Error('invalid mount');unmount=root.uiRenderer.mount(container)},
    dispose(){closing=true;return serialize(async()=>{if(disposed)return;disposed=true;unmount?.();unmount=undefined;try{for(const id of [...plugins.keys()].reverse())await remove(id)}finally{await root.fiber.dispose()}})},
  }
}

export {installTransport} from './transport.ts'

export {mountNativeSurface, surfaceDiagnostic} from './native-surface.ts'
