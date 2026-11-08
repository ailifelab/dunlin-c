#!/usr/bin/env python3
"""Real owned fixture file effect + injected result-commit failure; no replay on abandonment."""
import ctypes as C,json,sys,pathlib,tempfile,sqlite3,threading,http.server
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_delete_chat',[P,S],C.c_int),('dc_messages',[P,S],P),('dc_run',[P,S,S,S,P],P),('dc_workspace_request',[P,S],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
reviews=[]
@EV
def event(*a):pass
@AP
def approve(ctx,name,args):reviews.append((name.decode(),json.loads(args)));return 1
@CA
def cancel(_):return 0
hooks=Hooks(None,event,approve,cancel)
def val(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
requests=[]
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*a):pass
 def do_POST(self):
  q=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(q)
  if q['messages'][-1]['content']=='write once':msg={'role':'assistant','content':None,'tool_calls':[{'id':'owned-effect','type':'function','function':{'name':'write_file','arguments':json.dumps({'path':'effect.txt','text':'effect-once','expected_sha256':'new'})}}]};finish='tool_calls'
  else:msg={'role':'assistant','content':'No old command was replayed.'};finish='stop'
  body=json.dumps({'choices':[{'index':0,'message':msg,'finish_reason':finish}]}).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='dunlin-recovery-') as tmp:
 root=pathlib.Path(tmp);private=root/'private';private.mkdir(mode=0o700);work=root/'work';work.mkdir();db=lib.dc_open(str(private).encode());assert db;assert lib.dc_add_chat(db,b'chat',b'Chat',str(work).encode())
 def req(op,**kw):return val(lib.dc_workspace_request(db,json.dumps(dict(op=op,**kw)).encode()))
 sql=sqlite3.connect(private/'conversations.sqlite');sql.execute("CREATE TRIGGER owned_commit_failure BEFORE UPDATE OF state ON chats WHEN json_array_length(json_extract(NEW.state,'$.messages'))>=3 BEGIN SELECT RAISE(ABORT,'owned fixture failure');END");sql.commit()
 cfg=json.dumps({'base_url':f'http://127.0.0.1:{server.server_port}/v1','model':'fixture','protocol':'chat','stream':False,'desktop_tools':'ask'}).encode();r=val(lib.dc_run(db,b'chat',cfg,b'write once',C.byref(hooks)));assert r['status']=='unknown',r;assert (work/'effect.txt').read_text()=='effect-once';assert len(reviews)==1;before=(work/'effect.txt').stat().st_mtime_ns
 sql.execute('DROP TRIGGER owned_commit_failure');sql.commit();view=req('recovery.inspect',chat='chat')['data'];assert view['active'] and view['pending']=='owned-effect';assert not lib.dc_delete_chat(db,b'chat')
 assert not req('recovery.abandon',chat='chat',stateHash=view['stateHash'],acknowledgeUnknown=False)['ok'];assert not req('recovery.abandon',chat='chat',stateHash='stale',acknowledgeUnknown=True)['ok']
 r=req('recovery.abandon',chat='chat',stateHash=view['stateHash'],acknowledgeUnknown=True);assert r['ok'] and not r['data']['active'] and not r['data']['pending'] and r['data']['unknownCount']==1,r
 assert req('recovery.abandon',chat='chat',stateHash=view['stateHash'],acknowledgeUnknown=True)['ok'];assert len(requests)==1 and len(reviews)==1
 messages=val(lib.dc_messages(db,b'chat'));assert messages[-1]['observation']=='unknown' and 'UNKNOWN' in messages[-1]['content'];r=val(lib.dc_run(db,b'chat',cfg,b'continue safely',C.byref(hooks)));assert r['status']=='completed',r;assert len(reviews)==1 and (work/'effect.txt').stat().st_mtime_ns==before
 assert all('_dunlin_observation' not in m for m in requests[-1]['messages']);assert any('UNKNOWN external effect' in m.get('content','') for m in requests[-1]['messages'] if m['role']=='tool')
 traces=req('trace.segments',chat='chat')['data']['items'];assert {t['status'] for t in traces}=={'unknown','abandoned_unknown','completed'},traces
 original=next(t for t in traces if t['status']=='unknown');ended=next(t for t in traces if t['status']=='abandoned_unknown');assert original['run']==ended['run'];observations=req('trace.events',chat='chat',segment=ended['id'])['data']['items'];assert observations[0]['kind']=='recovery.abandoned' and observations[0]['body']['unknown_effect_retained']
 assert lib.dc_delete_chat(db,b'chat');assert sql.execute('SELECT count(*) FROM abandoned_effects').fetchone()[0]==1;assert sql.execute('SELECT count(*) FROM grant_records').fetchone()[0]==0;sql.close();lib.dc_close(db)
 print('PASS actual approved fixture write + failed result commit, unknown acknowledgement/CAS, paired host observations, idempotent abandon, new turn without re-execution, and minimal unknown guard retained after chat deletion')
server.shutdown();server.server_close()
