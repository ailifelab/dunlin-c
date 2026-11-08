"""Frozen run must not accept another public credential reference at same revision."""
import ctypes as C,json,pathlib,tempfile,threading,http.server,subprocess,sys
library=str(pathlib.Path(sys.argv[1]).resolve());lib=C.CDLL(library);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_run',[P,S,S,S,P],P),('dc_add_chat',[P,S,S,S],C.c_int)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
if len(sys.argv)>2 and sys.argv[2]=='--child':
 db=lib.dc_open(sys.argv[3].encode());assert db;print(value(lib.dc_run(db,b'chat',sys.argv[4].encode(),b'owned credential routing fixture',None)),flush=True);raise SystemExit(5)
requests=[];started=threading.Event();release=threading.Event()
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  requests.append(json.loads(self.rfile.read(int(self.headers['Content-Length']))))
  if len(requests)==1:started.set();release.wait(12)
  body=json.dumps(dict(choices=[dict(index=0,message=dict(role='assistant',content='fixture response'),finish_reason='stop')])).encode();self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers()
  try:self.wfile.write(body)
  except (BrokenPipeError,ConnectionResetError):pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='dunlin-credential-route-') as tmp:
 root=pathlib.Path(tmp);state=root/'private';state.mkdir(mode=0o700);work=root/'work';work.mkdir();db=lib.dc_open(str(state).encode());assert db;assert lib.dc_add_chat(db,b'chat',b'Fixture',str(work).encode());lib.dc_close(db)
 original=dict(base_url=f'http://127.0.0.1:{server.server_port}/v1',model='fixture',protocol='chat',stream=False,desktop_tools='noTools',desktop_profile_id='same-model',desktop_credential_revision=1,desktop_credential_id='credential-A',desktop_provider_id='provider-A')
 child=subprocess.Popen([sys.executable,__file__,library,'--child',str(state),json.dumps(original)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 assert started.wait(8);child.kill();child.communicate(timeout=5);release.set();db=lib.dc_open(str(state).encode());assert db
 for changes in [dict(desktop_credential_id='credential-B'),dict(desktop_provider_id='provider-B')]:
  before=len(requests);reply=value(lib.dc_run(db,b'chat',json.dumps(dict(original,**changes)).encode(),None,None));assert reply['status']=='busy' and len(requests)==before,reply
 omitted={k:v for k,v in original.items() if k not in ['desktop_credential_id','desktop_provider_id']};before=len(requests);reply=value(lib.dc_run(db,b'chat',json.dumps(omitted).encode(),None,None));assert reply['status']=='busy' and len(requests)==before,reply
 reply=value(lib.dc_run(db,b'chat',json.dumps(original).encode(),None,None));assert reply['status']=='completed' and len(requests)==2,reply
 lib.dc_close(db)
print('PASS genuinely killed owned run rejects changed/omitted provider or credential references before network; original reference resumes without duplicated user input or credentials')
server.shutdown()
