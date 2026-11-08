#!/usr/bin/env python3
"""Actual C writer interruption: model guards/DTOs use admitted identity, not UI preference."""
import ctypes as C,json,sys,pathlib,tempfile,threading,http.server,subprocess
P=C.c_void_p;S=C.c_char_p;library=pathlib.Path(sys.argv[1]).resolve();lib=C.CDLL(str(library))
for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_workspace_request',[P,S],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_run_with_secret',[P,S,S,S,P,S],P),('dc_chats',[P],P),('dc_messages',[P,S],P)]:
 fn=getattr(lib,name);fn.argtypes=args;fn.restype=result
def encode(v):return json.dumps(v).encode()
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
if '--child' in sys.argv:
 db=lib.dc_open(sys.argv[3].encode());assert db
 lib.dc_run_with_secret(db,b'chat',sys.argv[4].encode(),b'Frozen source model identity',None,b'');raise SystemExit('child returned before kill')
seen=[];entered=threading.Event();release=threading.Event();errors=[]
class Server(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  try:
   data=json.loads(self.rfile.read(int(self.headers['Content-Length'])));seen.append(data)
   if len(seen)==1:entered.set();release.wait(10)
   body=encode(dict(choices=[dict(index=0,message=dict(role='assistant',content='owned fixture'),finish_reason='stop')]))
   self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers()
   try:self.wfile.write(body)
   except (BrokenPipeError,ConnectionResetError):pass
  except BaseException as e:errors.append(e)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Server);threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='dunlin-active-model-route-') as tmp:
 root=pathlib.Path(tmp);state=root/'private';state.mkdir(mode=0o700);work=root/'work';work.mkdir();db=lib.dc_open(str(state).encode());assert db
 def req(op,**kw):return value(lib.dc_workspace_request(db,encode(dict(op=op,**kw))))
 def ok(op,**kw):
  reply=req(op,**kw);assert reply['ok'],reply;return reply.get('data')
 model=dict(id='model',name='Original name',endpoint=f'http://127.0.0.1:{server.server_port}/v1',model='fixture',protocolName='chat',maxOutput=4096,authentication='none')
 registry=ok('config.save',revision=0,models=[model],providers=[]);assert lib.dc_add_chat(db,b'chat',b'Chat',str(work).encode());assert ok('chats.metadata',id='chat')['model']==''
 settings=ok('config.model',id='model',revision=registry['revision']);settings['stream']=False;lib.dc_close(db)
 child=subprocess.Popen([sys.executable,'-B',__file__,str(library),'--child',str(state),json.dumps(settings)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 try:
  assert entered.wait(4),'owned writer did not reach fixture model';child.kill();child.communicate(timeout=3);assert child.returncode<0
 finally:
  release.set()
  if child.poll() is None:child.kill();child.communicate(timeout=3)
 db=lib.dc_open(str(state).encode());assert db
 reply=req('config.capabilities',id='model',revision=registry['revision'],contextLimit=8192,outputLimit=512,source='provider-model-inventory');assert not reply['ok'] and reply['error']=='configuration_active_route',('interrupted actual model route changed despite frozen intent',reply)
 assert ok('config.list')==registry;assert value(lib.dc_chats(db))[0]['model']=='model';assert ok('chats.metadata',id='chat')['model']=='model'
 renamed=dict(model,name='Display rename only');registry=ok('config.save',revision=registry['revision'],models=[renamed],providers=[])
 restored=ok('config.model',id='model',revision=registry['revision']);restored['stream']=False
 reply=value(lib.dc_run_with_secret(db,b'chat',encode(restored),None,None,b''));assert reply['status']=='completed',reply
 assert len(seen)==2 and seen[-1]['max_tokens']==4096 and seen[-1]['messages'][-1]['content']=='Frozen source model identity'
 messages=value(lib.dc_messages(db,b'chat'));assert len([m for m in messages if m['role']=='user'])==1
 registry=ok('config.capabilities',id='model',revision=registry['revision'],contextLimit=8192,outputLimit=512,source='provider-model-inventory');assert registry['models'][0]['maxOutput']==512
 lib.dc_close(db);assert not errors,errors
 print('PASS actual killed owned writer with no preferred model: frozen route blocks capability change, C list/metadata restore the admitted model, display rename remains allowed, exact original request resumes once and idle capability update succeeds')
server.shutdown();server.server_close()
