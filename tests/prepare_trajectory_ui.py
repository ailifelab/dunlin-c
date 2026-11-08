#!/usr/bin/env python3
"""Prepare a new named owned native UI fixture with genuine C/HTTP/tool events."""
import ctypes as C,pathlib,json,threading,http.server,sys,hashlib
root=pathlib.Path(__file__).resolve().parents[1];target=root/'build/v51/trajectory-ui';assert not target.exists(), 'never overwrite a running/existing acceptance identity'
target.mkdir(mode=0o700);work=root/'build/v51/Trajectory Fixture Project';assert not work.exists();work.mkdir()
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_workspace_request',[P,S],P),('dc_run',[P,S,S,S,P],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
@EV
def event(*_):pass
@AP
def approve(_,name,args):assert name==b'write_file';assert json.loads(args)['path']=='hello.txt';return 1
@CA
def cancel(_):return 0
hooks=Hooks(None,event,approve,cancel);requests=[]
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  req=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(req);last=req['messages'][-1]
  if last.get('content')=='请在验收目录写入 hello.txt，并确认执行结果。':
   msg={'role':'assistant','content':None,'tool_calls':[{'id':'fixture-write-01','type':'function','function':{'name':'write_file','arguments':json.dumps({'path':'hello.txt','text':'Dunlin C 轨迹验收。','expected_sha256':'new'},ensure_ascii=False)}}]};finish='tool_calls'
  else:msg={'role':'assistant','content':'已完成验收目录中的文件写入。授权决定、执行过程与持久结果分别记录在运行轨迹中。' if last['role']=='tool' else '切换模型后，当前聊天仍保留前一次写入及其工具结果。此回复由隔离 HTTP fixture 生成。'};finish='stop'
  body=json.dumps({'choices':[{'index':0,'message':msg,'finish_reason':finish}],'usage':{'prompt_tokens':137,'completion_tokens':31,'total_tokens':168}}).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
def val(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
db=lib.dc_open(str(target).encode());assert db
project={'op':'projects.create','id':'trace-project','name':'轨迹验收项目','path':str(work),'instructions':'仅用于隔离验收的 fixture 项目。','model':'model-a','policy':'ask','context':[],'expanded':True,'revision':0}
created=val(lib.dc_workspace_request(db,json.dumps(project).encode()));assert created['ok'],created;assert lib.dc_add_chat(db,b'trace-ui-chat','模型与工具轨迹验收'.encode(),str(work).encode())
endpoint=f'http://127.0.0.1:{server.server_port}/v1'
for model,text in [('model-a','请在验收目录写入 hello.txt，并确认执行结果。'),('model-b','现在切换模型，核对前一次文件操作的上下文。')]:
 cfg={'base_url':endpoint,'model':model,'protocol':'chat','stream':False,'desktop_profile_id':model,'desktop_tools':'ask'}
 assert val(lib.dc_run(db,b'trace-ui-chat',json.dumps(cfg).encode(),text.encode(),C.byref(hooks)))['status']=='completed'
profiles=[{'id':m,'name':'轨迹模型 '+m[-1].upper()+'（fixture）','endpoint':endpoint,'model':m,'protocolName':'chat','maxOutput':1024}for m in ('model-a','model-b')]
(target/'settings.json').write_text(json.dumps({'profiles':profiles,'profileID':'model-b','defaultProfileID':'model-a','theme':'light','projects':[],'showReasoning':False},ensure_ascii=False));(target/'settings.json').chmod(0o600)
assert (work/'hello.txt').read_text()=='Dunlin C 轨迹验收。';lib.dc_close(db);server.shutdown();server.server_close()
report={'scope':'Owned deterministic C + real loopback HTTP/tool fixture; not a real model','requests':len(requests),'library':{'path':sys.argv[1],'sha256':hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest()},'state':str(target),'effect_sha256':hashlib.sha256((work/'hello.txt').read_bytes()).hexdigest()}
(root/'reports/v51-completion-20261107/trajectory-ui-fixture.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,ensure_ascii=False))
