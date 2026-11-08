#!/usr/bin/env python3
"""Actual fixed Cordis/TypeScript process; does not alter global Node or user state."""
import ctypes as C,sys,pathlib,json,tempfile
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
lib.dc_dsh_prepare.argtypes=[S,S,S,C.c_int,P,C.POINTER(P),C.POINTER(P)];lib.dc_dsh_prepare.restype=P
lib.dc_worker_call.argtypes=[P,S,S,P];lib.dc_worker_call.restype=P;lib.dc_worker_close.argtypes=[P];lib.dc_free.argtypes=[P]
def take(p):
 assert p
 try:return C.string_at(p).decode()
 finally:lib.dc_free(p)
def val(p):return json.loads(take(p))
assets=pathlib.Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory(prefix='dunlin-dsh-') as tmp:
 root=pathlib.Path(tmp);root.chmod(0o700)
 e=P();actual=P();w=lib.dc_dsh_prepare(str(root).encode(),str(assets).encode(),b'/opt/homebrew/bin/node',0,None,C.byref(e),C.byref(actual));assert w,take(e) if e else 'missing error';assert take(actual).endswith('/bin/node')
 info=val(lib.dc_worker_call(w,b'system.info',b'{}',None));assert info['ok'] and info['data']['cordis']=='4.0.5' and info['data']['node']=='26.13.0'
 plugin=root/'plugin';plugin.mkdir();(plugin/'index.ts').write_text('''import type {Context} from '@deepseek-ai/cordis';
export const name='fixture';export const inject=['dunlin'];
export function apply(ctx:Context){ctx.dunlin.registerCommand(ctx,'hello',(args:{name:string})=>({text:'Hello '+args.name}));ctx.dunlin.contribute(ctx,'skin',{kind:'theme',name:'Fixture',tokens:{accent:'#7755ff'}})}
''')
 config={'id':'fixture','root':str(plugin),'entry':'index.ts','config':{}}
 r=val(lib.dc_worker_call(w,b'plugins.activate',json.dumps(config).encode(),None));assert r['ok'],r
 r=val(lib.dc_worker_call(w,b'plugins.contributions',b'{}',None));assert r['ok'] and r['data'][0]['kind']=='theme',r
 command=r['data'][0]['id'].split(':')[0]+':hello';r=val(lib.dc_worker_call(w,b'plugins.command',json.dumps({'plugin':'fixture','command':command,'arguments':{'name':'Dunlin'}}).encode(),None));assert r['data']['text']=='Hello Dunlin',r
 r=val(lib.dc_worker_call(w,b'plugins.deactivate',b'{"id":"fixture"}',None));assert r['ok'];assert val(lib.dc_worker_call(w,b'plugins.contributions',b'{}',None))['data']==[]
 lib.dc_worker_close(w)
 e=P();actual=P();w=lib.dc_dsh_prepare(str(root).encode(),str(root).encode(),b'/opt/homebrew/bin/node',0,None,C.byref(e),C.byref(actual));assert not w and take(e)=='compatibility_assets_invalid'
 print('PASS pinned assets, reuse exact system Node, real Cordis activation/disposal, TypeScript compilation, command/UI contribution and integrity failure')
