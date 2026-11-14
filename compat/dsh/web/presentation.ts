// Appearance only: verified C snapshots, never provider/tool/native authority.
const knobs=['--dsh-glass-blur','--dsh-glass-frost','--dsh-glass-brightness-black','--dsh-glass-brightness-white']
const seams:[string,string][]=[
 ['data-dsh-glass-frame','#root'],
 ['data-dsh-glass-sidebar-root','[class*="sidebarCol"] [class*="root"]'],
 ['data-dsh-glass-surface','button[class*="newSession"]'],
 ['data-dsh-glass-trajectory','[data-conversation-composer-overlay]'],
 ['data-dsh-glass-details','[class*="detailsCol"] [class*="root"]'],
 ['data-dsh-glass-inputbar',':has(> [data-composer-card])'],
 ['data-dsh-glass-add','[data-composer-card] [class*="add"]'],
 ['data-dsh-glass-stats','[data-slot="conversation.composer.dock"] [class*="root"]'],
]
export function installPresentation(root:any,options:any,selected:()=>{id:string,owner?:string}|undefined){
 let values:any[]=[],publisherCSS:any,previous='',publishTimer:any,closed=false,scheme='light'
 const styles=new Map<string,HTMLStyleElement>(),stamped=new Map<Element,Set<string>>(),fades:HTMLElement[]=[]
 const style=(name:string,css:string)=>{let node=styles.get(name);if(!css){node?.remove();styles.delete(name);return}if(!node){node=document.createElement('style');node.dataset.dunlinThemeEffects=name;styles.set(name,node);document.head.appendChild(node)}if(node.textContent!==css)node.textContent=css}
 const unstamp=()=>{for(const [element,attrs]of stamped)for(const attr of attrs)element.removeAttribute(attr);stamped.clear()}
 const stamp=()=>{if(!values.some(v=>v.glass.enabled))return;unstamp();for(const [attr,selector]of seams)for(const element of document.querySelectorAll(selector)){if(element.hasAttribute(attr))continue;element.setAttribute(attr,'');let attrs=stamped.get(element);if(!attrs){attrs=new Set();stamped.set(element,attrs)}attrs.add(attr)}}
 const apply=()=>{
  if(closed||options.presentationPublisher)return
  const matching=values.find(v=>v.preferences?.includes(selected()?.id)&&(!selected()?.owner||v.owner===selected()?.owner)),global=values[0],glass=global?.glass,doc=document.documentElement
  style('selected',matching?.selectedCSS??'');style('global',global?.globalCSS??'')
  if(matching?.glow)doc.dataset.dshGlow='on';else delete doc.dataset.dshGlow
  if(glass?.enabled){doc.dataset.dshGlass='on';doc.toggleAttribute('data-dsh-glass-float',glass.mode!=='compat');doc.toggleAttribute('data-dsh-glass-compat',glass.mode==='compat');doc.style.setProperty(knobs[0],glass.blur+'px');doc.style.setProperty(knobs[1],String(Math.min(glass.frost/50,1.4)));doc.style.setProperty(knobs[2],String(scheme==='dark'?Math.max(0,(50-glass.brightness)/50):0));doc.style.setProperty(knobs[3],String(scheme==='dark'?0:Math.max(0,(glass.brightness-50)/50)));if(!fades.length)for(const edge of ['top','bottom']){const span=document.createElement('span');span.dataset.dshGlassFade=edge;span.setAttribute('aria-hidden','true');document.body.appendChild(span);fades.push(span)}stamp()}
  else{delete doc.dataset.dshGlass;doc.removeAttribute('data-dsh-glass-float');doc.removeAttribute('data-dsh-glass-compat');for(const name of knobs)doc.style.removeProperty(name);for(const node of fades)node.remove();fades.length=0;unstamp()}
 }
 const observer=new MutationObserver(()=>{if(options.presentationPublisher)publish();else stamp()})
 observer.observe(options.presentationPublisher?document.documentElement:document.body,options.presentationPublisher?{attributes:true,attributeFilter:['data-dsh-glow','data-dsh-glass','style']}:{childList:true,subtree:true})
 function publish(){if(closed||!publisherCSS||!options.onPresentation)return;clearTimeout(publishTimer);publishTimer=setTimeout(()=>{
  if(closed)return;let raw:any={};try{raw=JSON.parse(localStorage.getItem('dsh-catppuccin:glass')??'{}')??{}}catch{}
  const number=(key:string,max:number,fallback:number)=>typeof raw[key]==='number'&&Number.isFinite(raw[key])?Math.max(0,Math.min(max,raw[key])):fallback
  const value={...publisherCSS,glow:document.documentElement.dataset.dshGlow==='on',glass:{enabled:document.documentElement.dataset.dshGlass==='on',mode:raw.mode==='compat'?'compat':'mica',blur:number('blur',40,2),frost:number('frost',100,20),brightness:number('brightness',100,50)}}
  const key=JSON.stringify(value);if(key===previous)return;previous=key;Promise.resolve(options.onPresentation(value)).catch(()=>options.onPresentationError?.())
 },60)}
 // This optional extension is installed only for the C-verified pinned theme.
 if(options.presentationPublisher)root.theme.registerPresentation=(css:any)=>{if(!css||typeof css.selectedCSS!=='string'||typeof css.globalCSS!=='string'||css.selectedCSS.length>65536||css.globalCSS.length>65536)throw Error('Invalid theme presentation');publisherCSS={selectedCSS:css.selectedCSS,globalCSS:css.globalCSS};publish()}
 const set=(next:any)=>{if(!Array.isArray(next)||next.length>32||next.some(v=>!v||typeof v.selectedCSS!=='string'||typeof v.globalCSS!=='string'||v.selectedCSS.length>65536||v.globalCSS.length>65536||typeof v.glow!=='boolean'||!v.glass||!Array.isArray(v.preferences)))throw Error('Invalid native presentation');values=next;apply()}
 root.on('theme/change',(snapshot:any)=>{scheme=snapshot.active?.colorScheme??'light';apply()})
 root.effect(()=>()=>{closed=true;clearTimeout(publishTimer);observer.disconnect();for(const node of styles.values())node.remove();styles.clear();for(const node of fades)node.remove();unstamp();if(!options.presentationPublisher){const doc=document.documentElement;delete doc.dataset.dshGlow;delete doc.dataset.dshGlass;doc.removeAttribute('data-dsh-glass-float');doc.removeAttribute('data-dsh-glass-compat');for(const key of knobs)doc.style.removeProperty(key)}})
 set(options.presentations??[])
 return {set,apply}
}
