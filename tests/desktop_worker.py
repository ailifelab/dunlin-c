#!/usr/bin/env python3
"""Actual isolated worker processes: framing, cancellation, fail-closed RPC and environment."""
import ctypes as C,sys,pathlib,tempfile,json,os,time
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_worker_open',[S,P,C.POINTER(P)],P),('dc_worker_call',[P,S,S,P],P),('dc_worker_close',[P],None),('dc_worker_capabilities',[P],P),('dc_free',[P],None)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',P),('approve',P),('cancelled',CA)]
stopped=False
@CA
def cancel(_):return int(stopped)
hooks=Hooks(None,None,None,cancel)
def val(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
with tempfile.TemporaryDirectory(prefix='dunlin-worker-') as tmp:
 root=pathlib.Path(tmp);root.chmod(0o700);script=root/'worker.py';script.write_text('''import os,sys,json,time
mode=sys.argv[1]
hello={'protocol':'dunlin.worker','major':1,'nonce':os.environ['DUNLIN_WORKER_NONCE'],'methods':['echo','null','error','hang','badid','duplicate','flood','exit']}
if mode=='badnonce':hello['nonce']='forged'
print(json.dumps(hello),flush=True)
for line in sys.stdin:
 q=json.loads(line);m=q['method'];r={'jsonrpc':'2.0','id':q['id']}
 if m=='echo':r['result']={'params':q['params'],'leaked':'DUNLIN_FIXTURE_PRIVATE_KEY' in os.environ}
 elif m=='null':r['result']=None
 elif m=='error':r['error']={'code':-32000,'message':'business failure'}
 elif m=='hang':time.sleep(10);r['result']='late'
 elif m=='badid':r['id']+=1;r['result']='forged'
 elif m=='duplicate':r['result']='ok';print(json.dumps(r),flush=True)
 elif m=='flood':print('x'*1100000,flush=True);continue
 elif m=='exit':sys.exit(3)
 print(json.dumps(r),flush=True)
''')
 os.environ['DUNLIN_FIXTURE_PRIVATE_KEY']='fixture-only-not-a-real-secret'
 def open_worker(mode='normal',timeout=1500):
  error=P();w=lib.dc_worker_open(json.dumps({'executable':sys.executable,'args':[str(script),mode],'home':str(root),'timeoutMs':timeout}).encode(),C.byref(hooks),C.byref(error));reason=C.string_at(error).decode() if error else ''
  if error:lib.dc_free(error)
  return w,reason
 def call(w,method,params={}):return val(lib.dc_worker_call(w,method.encode(),json.dumps(params).encode(),C.byref(hooks)))
 w,error=open_worker();assert w,error;assert val(lib.dc_worker_capabilities(w))['major']==1
 r=call(w,'echo',{'text':'中文'});assert r['ok'] and r['data']['params']['text']=='中文' and not r['data']['leaked'];assert call(w,'null')['ok'];assert call(w,'undeclared')['effectState']=='not_started';assert call(w,'echo')['ok'];assert call(w,'error')['effectState']=='reported_failure';assert call(w,'echo')['ok'];lib.dc_worker_close(w)
 w,error=open_worker('badnonce');assert not w and error=='worker_handshake_mismatch'
 for method,error in [('badid','worker_response_mismatch'),('flood','worker_frame_exceeds_limit'),('exit','worker_disconnected'),('hang','timeout')]:
  w,reason=open_worker(timeout=250);assert w,reason;r=call(w,method);assert not r['ok'] and r['error']==error and r['effectState']=='unknown',r;assert call(w,'echo')['effectState']=='not_started';lib.dc_worker_close(w)
 w,error=open_worker();assert w;stopped=True;r=call(w,'echo');assert not r['ok'] and r['effectState']=='not_started';lib.dc_worker_close(w);stopped=False
 print('PASS real Python process, nonce handshake, RPC data/null/error, method allowlist, environment scrub, invalid ID/overflow/exit/timeout, cancellation and no replay')
