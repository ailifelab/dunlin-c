"""Actual C process transport and actual Node sync adapter; no model commands."""
import ctypes as C,json,pathlib,sys,tempfile,time,os,signal
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
Event=C.CFUNCTYPE(None,P,S,S);Approve=C.CFUNCTYPE(C.c_int,P,S,S);Cancel=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',Event),('approve',Approve),('cancelled',Cancel)]
Native=C.CFUNCTYPE(P,P,S,P)
for name,args,result in [('dc_worker_open',[S,P,C.POINTER(P)],P),('dc_worker_call_native',[P,S,S,P,Native,P],P),('dc_worker_call',[P,S,S,P],P),('dc_worker_close',[P],None),('dc_worker_alive',[P],C.c_int),('dc_worker_capabilities',[P],P),('dc_free',[P],None)]:f=getattr(lib,name);f.argtypes=args;f.restype=result
allocator=C.CDLL(None);allocator.strdup.argtypes=[S];allocator.strdup.restype=P
count=0;effects=0;stop=False;child_pid=0
@Event
def event(*a):pass
@Approve
def approve(*a):raise AssertionError('No model approval belongs in transport test')
@Cancel
def cancelled(*a):return int(stop)
hooks=Hooks(None,event,approve,cancelled)
@Native
def dispatch(_,raw,_hooks):
 global count,effects,stop
 q=json.loads(raw);count+=1
 assert q['nativeCall']=='owned-call'
 if q['method']!='fixture.inspect':result={'ok':False,'error':'owned_native_refusal'}
 else:
  effects+=1;mode=q['arguments'].get('mode')
  if mode=='invalidResult':return allocator.strdup(b'[]')
  if mode=='resultTooLarge':return allocator.strdup(json.dumps({'data':'x'*1048576}).encode())
  if mode=='cancelAfter':stop=True
  if mode=='killAfter':os.kill(child_pid,signal.SIGKILL)
  result={'ok':True,'data':q['arguments']}
 return allocator.strdup(json.dumps(result,ensure_ascii=False).encode())
def take(raw):
 assert raw
 try:return json.loads(C.string_at(raw))
 finally:lib.dc_free(raw)
root=pathlib.Path.cwd();results=[]
with tempfile.TemporaryDirectory(prefix='dunlin-native-admission-') as tmp:
 for mode in ['normal','fragmented','refused','deferred','nonce','nulNonce','rpc','sequence','partial','oversize','flood','invalidResult','resultTooLarge','cancelAfter','killAfter','defaultOff']:
  count=effects=0;stop=False;home=pathlib.Path(tmp)/mode;home.mkdir(mode=0o700);failure=P()
  config={'executable':'/opt/homebrew/bin/node','args':[str(root/'tests/fixtures/native_admission_worker.mjs'),str(pathlib.Path(sys.argv[2]).resolve())],'home':str(home),'nativeBridge':mode!='defaultOff','timeoutMs':5000 if mode not in ['partial','oversize'] else 500}
  worker=lib.dc_worker_open(json.dumps(config).encode(),None,C.byref(failure));assert worker, C.string_at(failure) if failure else mode
  child_pid=take(lib.dc_worker_capabilities(worker))['runtime']['pid']
  try:
   response=take(lib.dc_worker_call_native(worker,b'test',json.dumps({'mode':'normal' if mode=='defaultOff' else mode}).encode(),C.byref(hooks),dispatch,None))
   if mode in ['normal','fragmented']:assert response['ok'] and response['data']['data']['text']=='本地 C admission' and count==effects==1,response
   elif mode=='refused':assert response['ok'] and not response['data']['ok'] and count==1 and effects==0 and lib.dc_worker_alive(worker),response
   elif mode=='deferred':
    assert response['ok'] and response['data']['scheduled'] and count==0,response
    time.sleep(.05);assert count==0,'callback executed while C was idle'
    result=take(lib.dc_worker_call_native(worker,b'ping',b'{}',None,dispatch,None));assert result['ok'] and result['data']['deferred']['ok'] and count==effects==1,result
   elif mode=='defaultOff':assert not response['ok'] and response['effectState']=='reported_failure' and count==effects==0 and lib.dc_worker_alive(worker),response
   else:
    assert not response['ok'] and response['effectState']=='unknown' and not lib.dc_worker_alive(worker),(mode,response)
    expected=128 if mode=='flood' else 1 if mode in ['invalidResult','resultTooLarge','cancelAfter','killAfter'] else 0
    assert count==expected,(mode,count,expected,response)
    after=take(lib.dc_worker_call_native(worker,b'test',b'{"mode":"normal"}',None,dispatch,None));assert not after['ok'] and after['effectState']=='not_started' and count==expected,'interrupted admission replayed'
   results.append({'case':mode,'callbacks':count,'effects':effects,'state':response['effectState']})
  finally:lib.dc_worker_close(worker)
print(json.dumps({'cases':results,'scope':'C/Node private protocol only; no SDK agent proxy/model/native UI acceptance'},indent=2))
print('PASS actual optional C/Node sync admission: Unicode/fragmentation/refusal/deferred wake; nonce/NUL/RPC/sequence/size/budget/cancel/disconnect/invalid reply fail closed without replay; default-off unaffected')
