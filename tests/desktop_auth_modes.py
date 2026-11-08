"""Explicit anonymous routes never send a Keychain/host/env credential."""
import ctypes as C,json,pathlib,tempfile,sys,threading,http.server,os
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_run',[P,S,S,S,P],P),('dc_run_with_secret',[P,S,S,S,P,S],P),('dc_probe_model',[S,S,P],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
seen=[]
class Server(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_GET(self):
  seen.append(dict(method='GET',headers=dict(self.headers)));body=json.dumps(dict(data=[dict(id='fixture')])).encode();self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
 def do_POST(self):
  request=json.loads(self.rfile.read(int(self.headers['Content-Length'])));seen.append(dict(method='POST',headers=dict(self.headers)));body=json.dumps(dict(choices=[dict(index=0,message=dict(role='assistant',content='fixture only'),finish_reason='stop')])).encode();self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Server);threading.Thread(target=server.serve_forever,daemon=True).start()
token='OWNED_FIXTURE_TOKEN_NOT_A_REAL_CREDENTIAL';env_token='OWNED_ENV_TOKEN_DIFFERENT_FROM_HOST';key='DUNLIN_OWN_AUTH_MODE_FIXTURE';assert key not in os.environ;os.environ[key]=env_token
with tempfile.TemporaryDirectory(prefix='dunlin-auth-mode-') as tmp:
 root=pathlib.Path(tmp);state=root/'private';state.mkdir(mode=0o700);work=root/'work';work.mkdir();db=lib.dc_open(str(state).encode());assert db
 for chat in ['anonymous','legacy-none','explicit-auto']:assert lib.dc_add_chat(db,chat.encode(),chat.encode(),str(work).encode())
 cfg=dict(base_url=f'http://127.0.0.1:{server.server_port}/v1',model='fixture',protocol='chat',stream=False,desktop_tools='noTools',desktop_auth='none',key_env=key)
 raw=json.dumps(cfg).encode();reply=value(lib.dc_run_with_secret(db,b'anonymous',raw,b'owned test',None,b''));assert reply['status']=='completed',reply;assert 'Authorization' not in seen[-1]['headers']
 reply=value(lib.dc_run(db,b'legacy-none',raw,b'owned test',None));assert reply['status']=='completed' and 'Authorization' not in seen[-1]['headers'],reply
 inventory=value(lib.dc_probe_model(raw,b'',None));assert inventory['ok'] and 'Authorization' not in seen[-1]['headers'],inventory
 before=len(seen);reply=value(lib.dc_run_with_secret(db,b'anonymous',raw,b'owned test',None,token.encode()));assert reply['status']=='failed' and len(seen)==before,reply
 inventory=value(lib.dc_probe_model(raw,token.encode(),None));assert not inventory['ok'] and len(seen)==before
 automatic=json.dumps(dict(cfg,desktop_auth='auto')).encode();reply=value(lib.dc_run_with_secret(db,b'explicit-auto',automatic,b'owned test',None,token.encode()));assert reply['status']=='completed' and seen[-1]['headers'].get('Authorization')=='Bearer '+token
 gemini=json.dumps(dict(cfg,protocol='gemini',desktop_auth='bearer')).encode();inventory=value(lib.dc_probe_model(gemini,token.encode(),None));assert inventory['ok'] and seen[-1]['headers'].get('Authorization')=='Bearer '+token and 'x-goog-api-key' not in seen[-1]['headers'],inventory
 assert os.environ[key]==env_token
 lib.dc_close(db)
del os.environ[key];server.shutdown()
print('PASS explicit none ignores legacy env reference, accepts empty host key, refuses conflicting nonempty host key before requests; explicit auto still sends only the provided fixture key without changing environment')
