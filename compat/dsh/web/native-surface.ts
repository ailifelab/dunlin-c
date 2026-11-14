import {installPetAlpha} from './pet-alpha'
// The platform owns the WebView, isolated origin, CSP and RPC admission.
// Each view has a private JS realm, including its Storage prototype.
type Request = (method: string, args: Record<string, unknown>) => Promise<any>

export function surfaceDiagnostic(event: any): string {
  const kind = event?.error?.name ?? event?.reason?.name ?? event?.name
  const type = ['TypeError','RangeError','ReferenceError','SyntaxError'].includes(kind) ? kind : 'Error'
  return type + ' line ' + (Number.isSafeInteger(event?.lineno) ? event.lineno : 0)
}

async function restoreStorage(request: Request, fault: (error: unknown) => void) {
  const initial = await request('storage.read', {})
  for (const [key,value] of Object.entries(initial)) localStorage.setItem(key, String(value))
  const prototype = Storage.prototype
  const {setItem,removeItem,clear} = prototype
  let pending = Promise.resolve()
  function persist() {
    const value: Record<string,string> = Object.create(null)
    for (let i=0; i<localStorage.length; i++) {
      const key = localStorage.key(i)
      if (key !== null) value[key] = localStorage.getItem(key)!
    }
    pending = pending.then(() => request('storage.write', {value})).catch(fault)
  }
  prototype.setItem = function(key,value) { setItem.call(this,key,value); persist() }
  prototype.removeItem = function(key) { removeItem.call(this,key); persist() }
  prototype.clear = function() { clear.call(this); persist() }
}

export async function mountNativeSurface(runtime: any, registration: unknown, options: any,
                                        request: Request, fault: (error: unknown) => void,
                                        onTheme: (theme: string) => void) {
  // WebKit versions differ; third-party clients may use idle callbacks for focus or cleanup.
  if (typeof window.requestIdleCallback !== 'function') {
    window.requestIdleCallback = callback => window.setTimeout(() => {
      const start = performance.now()
      callback({didTimeout:false,timeRemaining:()=>Math.max(0,8-(performance.now()-start))})
    }, 0)
    window.cancelIdleCallback = handle => window.clearTimeout(handle)
  }
  if (options.package === 'dsh-pet') installPetAlpha()
  const workbench = options.slot === 'dunlin.workbench'
  if (!workbench && options.slot !== 'shell.overlay') await restoreStorage(request, fault)
  const native = workbench ? await request('native.snapshot', {}) : undefined
  const surface = await runtime.createSurface(options.chat, fault, {
    ...options, native, request, onTheme, onPresentationError: fault,
    onPresentation: (value: unknown) => request('theme.presentation', {value})
  })
  const page = globalThis as any
  page.surface = surface
  await surface.load(registration, options.package, options.config)
  if (page.__dunlinConfiguration !== undefined) surface.setConfiguration(page.__dunlinConfiguration)
  surface.mount(document.getElementById('surface'))
}
