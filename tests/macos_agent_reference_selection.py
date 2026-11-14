#!/usr/bin/env python3
"""Exercise native macOS ordinary/project turns and an unselected scheduled worker against an owned HTTP peer."""
import pathlib,json,ctypes as C,subprocess,sys,tempfile,threading,http.server,os,hashlib
# argv[1] is the shared C test library built by scripts/build-desktop-test-macos.py.
# A second argument can reuse an already built copy of the native proof runner.
here=pathlib.Path(__file__).resolve().parent
repo=here.parent
lib_path=pathlib.Path(sys.argv[1]).resolve()
work=tempfile.TemporaryDirectory(prefix="dunlin-agent-reference-")
d=pathlib.Path(work.name)
proof=pathlib.Path(sys.argv[2]).resolve() if len(sys.argv)>2 else d/"proof"
if len(sys.argv)==2:
    sources=[str(p) for p in (repo/"dunlin-copilot/macos/Sources/DunlinApp").glob("*.swift") if "@main" not in p.read_text()]
    subprocess.run(["xcrun","swiftc","-parse-as-library","-O","-whole-module-optimization","-num-threads","4",
        "-swift-version","5","-target","arm64-apple-macos26.0","-I",str(repo/"dunlin-copilot/macos/Sources/CDesktop"),
        *sources,str(here/"macos_agent_reference_selection.swift"),str(lib_path),"-o",str(proof)],check=True)
peer_arrivals=[]
def enc(v):return json.dumps(v,ensure_ascii=False).encode()
lib=C.CDLL(str(lib_path))
P=C.c_void_p;S=C.c_char_p
for name,args,ret in [('dc_open',[S],P),('dc_close',[P],None),('dc_workspace_request',[P,S],P),
   ('dc_add_chat',[P,S,S,S],C.c_int),('dc_free',[P],None),('dc_identity_open',[S],P),('dc_identity_close',[P],None),
   ('dc_identity_request',[P,S],P),('dc_bind_identity',[P,S],C.c_int)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=ret
def reply(fn,db,body):
 p=fn(db,enc(body));assert p
 try:r=json.loads(C.string_at(p))
 finally:lib.dc_free(p)
 assert r['ok'],r;return r.get('data')
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  q=json.loads(self.rfile.read(int(self.headers['Content-Length'])));assert q['model']=='fixture-native'
  assert not self.headers.get('Authorization')
  m=q['messages'][-1]['content'];word=m.strip().split('\n')[0]
  peer_arrivals.append((word,'FILE_MARKER_6f59' in m,len(q.get('tools') or [])))
  value='Reply '+word
  if q.get('stream'):
   body=b'data: '+enc(dict(choices=[dict(index=0,delta=dict(role='assistant',content=value),finish_reason='stop')]))+b'\n\ndata: [DONE]\n\n';ct='text/event-stream'
  else:
   body=enc(dict(choices=[dict(index=0,message=dict(role='assistant',content=value),finish_reason='stop')]));ct='application/json'
  self.send_response(200);self.send_header('Content-Type',ct);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
try:
 with tempfile.TemporaryDirectory(dir=d,prefix='state-') as name:
  root=pathlib.Path(name);state=root/'data';state.mkdir(mode=0o700)
  identity=lib.dc_identity_open(os.fsencode(state)); rows=reply(lib.dc_identity_request,identity,dict(op='list'));actual=pathlib.Path(next(r['directory'] for r in rows if r['selected']));lib.dc_identity_close(identity)
  db=lib.dc_open(os.fsencode(actual));assert db and lib.dc_bind_identity(db,b'local')
  req=lambda op,**kw:reply(lib.dc_workspace_request,db,dict(op=op,**kw))
  proj=root/'project';ordinary=root/'ordinary';proj.mkdir();ordinary.mkdir()
  source=b'FILE_MARKER_6f59';(proj/'reference.txt').write_bytes(source);sha=hashlib.sha256(source).hexdigest()
  endpoint='http://127.0.0.1:%s/v1'%server.server_port
  registry=req('config.save',revision=0,providers=[],models=[dict(id='fixture',name='Fixture',endpoint=endpoint,model='fixture-native',protocolName='chat',maxOutput=256,authentication='none')])
  req('projects.create',id='project',name='Example',path=str(proj),instructions='',model='fixture',context=['reference.txt'],contextVersions={'reference.txt':sha},policy='ask')
  for chat,path in [('direct',proj),('ordinary',ordinary)]:
   assert lib.dc_add_chat(db,chat.encode(),chat.encode(),os.fsencode(path))
   req('chats.update',id=chat,field='model',value='fixture')
  req('agents.create',id='worker',project='project',chat='',model='fixture',task='worker-test',parent='',revision=registry['revision'])
  lib.dc_close(db)
  env=dict(os.environ)
  proc=subprocess.run([str(proof),'--data-dir',str(state)],timeout=100,env=env)
  if proc.returncode:
   print('request evidence',peer_arrivals);raise SystemExit(proc.returncode)
  assert [x[0] for x in peer_arrivals]==['ordinary','no-selection','selected','worker-test'],peer_arrivals
  assert [x[1] for x in peer_arrivals[:3]]==[False,False,True],peer_arrivals
  assert peer_arrivals[0][2]==0 and peer_arrivals[1][2]>0
  print('request evidence',peer_arrivals,flush=True)
  assert not peer_arrivals[3][1],'unsolicited pinned reference observed in actual scheduled-worker HTTP request'
  print('PASS pinned file only transmitted after explicit interactive selection')
finally:server.shutdown();server.server_close();work.cleanup()
