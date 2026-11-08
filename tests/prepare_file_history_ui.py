#!/usr/bin/env python3
"""New isolated native fixture; never overwrites a prior identity or user project."""
import ctypes as C,pathlib,json,threading,http.server,sys,hashlib
root=pathlib.Path(__file__).resolve().parents[1];target=root/'build/v51/file-history-ui';work=root/'build/v51/File History Fixture Project';assert not target.exists() and not work.exists();target.mkdir(mode=0o700);work.mkdir()
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_workspace_request',[P,S],P),('dc_run',[P,S,S,S,P],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
@EV
def event(*_):pass
@AP
def approve(_,name,args):assert name==b'write_file' and json.loads(args)['path']=='greeting.txt';return 1
@CA
def cancel(_):return 0
hooks=Hooks(None,event,approve,cancel);requests=[]
versions=['第一版：Dunlin C 文件回退验收。','第二版：文件历史由 C 回执提供。']
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  req=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(req);last=req['messages'][-1]
  if last['role']=='user':
   version=0 if last['content']=='请创建第一版 greeting.txt。' else 1;expected='new' if version==0 else hashlib.sha256(versions[0].encode()).hexdigest();args={'path':'greeting.txt','text':versions[version],'expected_sha256':expected}
   msg={'role':'assistant','content':None,'tool_calls':[{'id':'fixture-write-'+str(version),'type':'function','function':{'name':'write_file','arguments':json.dumps(args,ensure_ascii=False)}}]};finish='tool_calls'
  else:msg={'role':'assistant','content':'已根据实际工具结果完成文件写入。可以从右侧文件变更检查持久快照，预览后手动回退。'};finish='stop'
  body=json.dumps({'choices':[{'index':0,'message':msg,'finish_reason':finish}],'usage':{'prompt_tokens':123,'completion_tokens':41,'total_tokens':164}}).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
def val(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
db=lib.dc_open(str(target).encode());assert db
project={'op':'projects.create','id':'file-history-project','name':'文件回退验收项目','path':str(work),'instructions':'仅操作独立验收目录，所有数据均为 deterministic fixture。','model':'file-fixture','policy':'ask','context':[],'expanded':True,'revision':0};assert val(lib.dc_workspace_request(db,json.dumps(project).encode()))['ok'];assert lib.dc_add_chat(db,b'file-history-ui-chat','文件修改与回退验收'.encode(),str(work).encode())
endpoint=f'http://127.0.0.1:{server.server_port}/v1';cfg=json.dumps({'base_url':endpoint,'model':'fixture-local','protocol':'chat','stream':False,'desktop_profile_id':'file-fixture','desktop_tools':'ask'}).encode()
for prompt in ['请创建第一版 greeting.txt。','请写入第二版，保留可回退记录。']:assert val(lib.dc_run(db,b'file-history-ui-chat',cfg,prompt.encode(),C.byref(hooks)))['status']=='completed'
assert val(lib.dc_workspace_request(db,b'{"op":"chats.update","id":"file-history-ui-chat","field":"model","value":"file-fixture"}'))['ok'];receipts=val(lib.dc_workspace_request(db,b'{"op":"receipts.list","chat":"file-history-ui-chat"}'))['data']['items'];assert len(receipts)==2 and all(r['state']=='applied' for r in receipts)
profile={'id':'file-fixture','name':'项目验收模型（离线 fixture）','endpoint':endpoint,'model':'fixture-local','protocolName':'chat','maxOutput':1024};(target/'settings.json').write_text(json.dumps({'profiles':[profile],'profileID':'file-fixture','defaultProfileID':'file-fixture','theme':'light','projects':[],'showReasoning':False},ensure_ascii=False));(target/'settings.json').chmod(0o600)
assert (work/'greeting.txt').read_text()==versions[1];lib.dc_close(db);server.shutdown();server.server_close()
report={'scope':'Owned loopback model/protocol fixture + actual C authorized file writes, not a real model','state':str(target),'project':str(work),'requests':len(requests),'library_sha256':hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest(),'versions':versions,'receipts':[r['id'] for r in receipts]};(root/'reports/v51-completion-20261107/file-history-ui-fixture.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n');print('Prepared 2 C receipts and isolated native project; no credentials or live services used.')
