#!/usr/bin/env python3
"""Retired 16-character checkpoints must reject without model effects.
Both libraries and model fixtures are owned; only an owned child is killed.
"""
import desktop_fixture_sqlite as fixture_sqlite
import ctypes as C,json,pathlib,sys,tempfile,threading,http.server,subprocess,sqlite3
P=C.c_void_p;S=C.c_char_p
def load(path):
 lib=C.CDLL(str(pathlib.Path(path).resolve()))
 for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_run_with_secret',[P,S,S,S,P,S],P)]:
  fn=getattr(lib,name);fn.argtypes=args;fn.restype=result
 return lib
def value(lib,p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
token=b'OWNED_FAKE_RESUME_TOKEN'
if '--child' in sys.argv:
 lib=load(sys.argv[1]);db=lib.dc_open(sys.argv[3].encode());assert db
 lib.dc_run_with_secret(db,b'chat',sys.argv[4].encode(),b'Original previous-image message',None,token)
 raise SystemExit('child returned before kill')
current=sys.argv[1];old=current;lib=load(current);seen=[];hold=False;entered=threading.Event();release=threading.Event();errors=[]
class Server(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  try:
   data=json.loads(self.rfile.read(int(self.headers['Content-Length'])));seen.append((data,self.headers.get('Authorization')))
   if hold:entered.set();release.wait(10)
   body=json.dumps(dict(choices=[dict(index=0,message=dict(role='assistant',content='owned response'),finish_reason='stop')])).encode();self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers()
   try:self.wfile.write(body)
   except (BrokenPipeError,ConnectionResetError):pass
  except BaseException as e:errors.append(e)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Server);threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='dunlin-old-host-resume-') as tmp:
 for kind in ['legacy-v1']:
  state=pathlib.Path(tmp)/kind;state.mkdir(mode=0o700);work=pathlib.Path(tmp)/(kind+'-work');work.mkdir();old_api=load(old);db=old_api.dc_open(str(state).encode());assert db;assert old_api.dc_add_chat(db,b'chat',b'Chat',str(work).encode());old_api.dc_close(db)
  cfg=dict(base_url=f'http://127.0.0.1:{server.server_port}/v1',model='fixture',protocol='chat',stream=False,max_tokens=64,key_env='IGNORED_OLD_REFERENCE',system_prompt='Original frozen system')
  if kind=='frozen':cfg.update(desktop_tools='noTools',desktop_profile_id='fixture-profile',desktop_credential_revision=0)
  raw=json.dumps(cfg);entered.clear();release.clear();hold=True
  child=subprocess.Popen([sys.executable,'-B',__file__,old,'--child',str(state),raw],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  try:
   assert entered.wait(4),'owned prior-image child did not reach fixture HTTP'
   child.kill();child.communicate(timeout=3);assert child.returncode<0
  finally:
   hold=False;release.set()
   if child.poll() is None:child.kill();child.communicate(timeout=3)
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:
   checkpoint=json.loads(sql.execute('SELECT state FROM chats').fetchone()[0]);assert checkpoint['active']
   if kind=='legacy-v1':
    # This prior desktop image already writes 64-digit profiles. Construct the
    # documented older v1 metadata as a separate controlled migration input;
    # this is not a claim that the prior image itself emitted the v1 form.
    assert len(checkpoint['_dunlin_profile'])==64
    legacy_hash=1469598103934665603
    for byte in raw.encode():legacy_hash=((legacy_hash^byte)*1099511628211)&((1<<64)-1)
    checkpoint['_dunlin_profile']=f'{legacy_hash:016x}'
    checkpoint.pop('_dunlin_run',None);sql.execute('UPDATE chats SET state=?',(json.dumps(checkpoint),));sql.execute('DELETE FROM run_records')
  db=lib.dc_open(str(state).encode());assert db;before=len(seen)
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:before_source=sql.execute('SELECT state FROM chats').fetchone()[0]
  result=value(lib,lib.dc_run_with_secret(db,b'chat',raw.encode(),None,None,token))
  assert result['status'] in ['failed','busy'] and len(seen)==before,('obsolete checkpoint resumed',result)
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:assert sql.execute('SELECT state FROM chats').fetchone()[0]==before_source,'rejection changed original checkpoint'
  lib.dc_close(db)
  print('PASS retired checkpoint rejected before HTTP and original source preserved')
assert not errors,errors;server.shutdown();server.server_close()
