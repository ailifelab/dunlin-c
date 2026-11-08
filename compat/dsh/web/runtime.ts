// Optional DSH browser compatibility. No filesystem, credentials, model/tool
// dispatch or persistence live here; the C/native owner supplies approved modules.
import * as React from 'react'
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

export const supportedSlots = Object.freeze(['conversation.composer.dock','settings.general.item'])
const baseline = new Map<string,unknown>([
  ['@deepseek-ai/dsh-client-runtime/client',Object.freeze({defineStore:Store.defineStore})],
  ['react',React],['react/jsx-runtime',JSXRuntime],['@deepseek-ai/dsh-client-store',Store],['@deepseek-ai/dsh-client-ui-primitives',Primitives],['react-dom',ReactDOM],['react-dom/client',ReactDOMClient],
  ['@deepseek-ai/cordis',Cordis],['@deepseek-ai/dsh-client-ui-slots',Slots],
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
  root.on('theme/change',(snapshot:any)=>{
    if(typeof document==='undefined')return;
    document.body.dataset.dsDarkTheme=String(snapshot.active?.colorScheme==='dark');
    for(const [key,value] of Object.entries(snapshot.active?.tokens ?? {}))if(key.startsWith('--dsw-')&&typeof value==='string')document.body.style.setProperty(key,value);
    if(ready)options.onTheme?.(snapshot.preference);
  });
  const selectedSlot=supportedSlots.includes(options.slot)?options.slot:'conversation.composer.dock';
  const binding=Object.freeze({key:sessionID,ctx:root,hooks:{},keyedHooks:{},props:{}})
  const absent=Object.freeze({key:undefined,hooks:{},keyedHooks:{},props:{}})
  const observable=(value:any)=>({getSnapshot:()=>value,subscribe:()=>()=>{}})
  await root.plugin({name:'dunlin-surface',inject:['slots'],apply(ctx:any){
    ctx.slots.installScope('session',{
      current:observable(binding),
      renderArea:(current:any,props:any)=>current.key===sessionID ? props.children:(props.empty?.() ?? null),
      bindingSource:(target:any)=>observable(target===sessionID || target?.id===sessionID ? binding:absent),
    })
    ctx.slots.register({name:'root',id:'dunlin-surface',children:{
      'conversation.composer.dock':{kind:'list',scope:'session'},'settings.general.item':{kind:'list',scope:'root'},
    }},(props:any)=>React.createElement(props.SessionProvider,null,
      React.createElement('div',{'data-dunlin-surface':sessionID},props.renderSlot(selectedSlot,{}))))
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
      const names=Array.isArray(inject)?inject:Object.keys(inject)
      if(names.some((name:string)=>!['slots','locale','theme'].includes(name)))throw Error('unsupported client service')
      fiber=await root.plugin({...module,name:expectedID},config)
      if(!fiber || fiber.state!==Cordis.FiberState.ACTIVE || root.slots.entries('root').length!==1)throw Error('client activation failed')
      plugins.set(expectedID,fiber);if(options.theme)root.theme.setTheme(options.theme);ready=true
    }catch(error){
      if(fiber)await fiber.dispose()
      report({plugin:expectedID,phase:'activate',code:'client_activation_failed'})
      throw error
    }
  }
  return {
    load:(registration:Registration,id:string,config:Record<string,unknown>={})=>closing?Promise.reject(Error('surface disposed')):serialize(()=>load(registration,id,config)),
    setTheme:(id:string)=>{if(disposed)throw Error('surface disposed');root.theme.setTheme(id)},
    remove:(id:string)=>serialize(()=>remove(id)),
    render:()=>{if(disposed)throw Error('surface disposed');return root.slots.renderSlot('root',{})},
    entries:(slot:string)=>disposed?[]:root.slots.entries(slot).map((x:any)=>({id:x.options.id,registrant:x.registrant})),
    mount(container:HTMLElement){if(disposed||unmount)throw Error('invalid mount');unmount=root.uiRenderer.mount(container)},
    dispose(){closing=true;return serialize(async()=>{if(disposed)return;disposed=true;unmount?.();unmount=undefined;try{for(const id of [...plugins.keys()].reverse())await remove(id)}finally{await root.fiber.dispose()}})},
  }
}
