#!/usr/bin/env python3
"""Durable exact-call approval audit, C fail-closed storage and lifecycle checks.
A deterministic owned model fixture only; never approves live model requests.
"""
import ctypes as C,json,pathlib,tempfile,sys,sqlite3,hashlib,threading,http.server,time
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_workspace_request',[P,S],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_delete_chat',[P,S],C.c_int),('dc_run',[P,S,S,S,P],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
allow=True;reviewed=[]
@EV
def event(*a):pass
@AP
def approve(ctx,name,args):reviewed.append(args.decode());time.sleep(.05);return int(allow)
@CA
def cancel(*a):return 0
hooks=Hooks(None,event,approve,cancel)
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*a):pass
 def do_POST(self):
  r=json.loads(self.rfile.read(int(self.headers['Content-Length'])));last=r['messages'][-1]
  if last['role']=='user':msg={'role':'assistant','content':None,'tool_calls':[{'id':'c1','type':'function','function':{'name':'write_file','arguments':json.dumps({'path':'audit.txt','text':last['content']})}}]};finish='tool_calls'
  else:msg={'role':'assistant','content':'finished'};finish='stop'
  b=json.dumps({'choices':[{'index':0,'message':msg,'finish_reason':finish}]}).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(b)));self.end_headers();self.wfile.write(b)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
def val(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
with tempfile.TemporaryDirectory(prefix='dunlin-grants-') as td:
 root=pathlib.Path(td);state=root/'state';state.mkdir(mode=0o700);work=root/'work';work.mkdir();db=lib.dc_open(str(state).encode());assert db
 cfg=json.dumps({'base_url':f'http://127.0.0.1:{server.server_port}/v1','model':'fixture','protocol':'chat','stream':False}).encode()
 def run(id,input):return val(lib.dc_run(db,id.encode(),cfg,input.encode(),C.byref(hooks)))
 def request(op,**kw):
  r=val(lib.dc_workspace_request(db,json.dumps(dict(op=op,**kw)).encode()));assert r['ok'],r;return r.get('data')
 assert lib.dc_add_chat(db,b'allowed',b'allowed',str(work).encode());assert run('allowed','approved-fixture')['status']=='completed';assert (work/'audit.txt').read_text()=='approved-fixture'
 g=request('grants.list',chat='allowed')[0];assert g['allowed'] and g['status']=='expired' and g['waitMs']>=40 and g['ended']>=g['decided']>=g['started'];assert g['argsHash']==hashlib.sha256(reviewed[-1].encode()).hexdigest()
 assert request('grants.revoke',id=g['id'])['state']=='already_inactive';assert (work/'audit.txt').read_text()=='approved-fixture'
 allow=False;assert lib.dc_add_chat(db,b'denied',b'denied',str(work).encode());assert run('denied','denied-fixture')['status']=='denied';assert (work/'audit.txt').read_text()=='approved-fixture';d=request('grants.list',chat='denied')[0];assert not d['allowed'] and d['status']=='denied'
 print('PASS persistent exact-argument approval/denial, actual effect, duration, inactive revocation does not revert file')
 sql=sqlite3.connect(state/'conversations.sqlite');runs=sql.execute('SELECT id,snapshot,status FROM run_records').fetchall();assert len(runs)==2;assert all(len(x[0])==64 and json.loads(x[1])['model']=='fixture' for x in runs)
 sql.execute("CREATE TRIGGER deny_audit BEFORE INSERT ON grant_records BEGIN SELECT RAISE(ABORT,'owned storage fault'); END");sql.commit();n=len(reviewed)
 assert lib.dc_add_chat(db,b'storage-fault',b'storage fault',str(work).encode());result=run('storage-fault','must not write');print('Storage fault result:',result,flush=True);assert result['status']=='denied';assert len(reviewed)==n and (work/'audit.txt').read_text()=='approved-fixture'
 sql.execute('DROP TRIGGER deny_audit');sql.commit();allow=True
 sql.execute("CREATE TRIGGER deny_grant_end BEFORE UPDATE OF status ON grant_records WHEN NEW.status='expired' BEGIN SELECT RAISE(ABORT,'owned finalize fault'); END");sql.commit()
 assert lib.dc_add_chat(db,b'finalize-fault',b'finalize fault',str(work).encode())
 final=run('finalize-fault','effect-before-audit-failure');assert final['status']=='unknown',final
 assert (work/'audit.txt').read_text()=='effect-before-audit-failure' and '拒绝' not in final['text']
 sql.execute('DROP TRIGGER deny_grant_end');sql.execute("INSERT INTO grant_records(chat,tool,args,args_hash,status) VALUES('interrupted','shell','fixture','hash','awaiting')");sql.commit();sql.close();lib.dc_close(db)
 db=lib.dc_open(str(state).encode());assert db;assert request('grants.list',chat='allowed')[0]['argsHash']==g['argsHash'];assert request('grants.list',chat='interrupted')[0]['status']=='interrupted'
 assert lib.dc_delete_chat(db,b'allowed');assert request('grants.list',chat='allowed')==[]
 sql=sqlite3.connect(state/'conversations.sqlite');assert sql.execute("SELECT count(*) FROM run_records WHERE chat='allowed'").fetchone()[0]==0;sql.close();lib.dc_close(db)
 print('PASS grant persistence failure prevents effect; run snapshot/checkpoint association; startup invalidation and delete cleanup')
server.shutdown();server.server_close()
