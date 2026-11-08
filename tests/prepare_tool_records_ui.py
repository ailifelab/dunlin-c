#!/usr/bin/env python3
"""Prepare new owned UI state from real C effects and an explicit local fixture."""
import ctypes as C,pathlib,json,threading,http.server,sys,hashlib
root=pathlib.Path(__file__).resolve().parents[1];state=root/'build/v51/tool-records-ui';work=root/'build/v51/Tool Records Fixture Project'
assert not state.exists() and not work.exists();state.mkdir(mode=0o700);work.mkdir()
library=pathlib.Path(sys.argv[1]).resolve();lib=C.CDLL(str(library));P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_workspace_request',[P,S],P),('dc_run',[P,S,S,S,P],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
allow=True
@EV
def event(*_):pass
@AP
def approve(_,name,args):
 a=json.loads(args);assert name in (b'write_file',b'shell');assert a.get('path') in (None,'sample.txt','denied.txt');return int(allow)
@CA
def cancel(_):return 0
hooks=Hooks(None,event,approve,cancel);requests=[]
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  request=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(request);last=request['messages'][-1];calls=[]
  if last['role']=='user':
   prompt=last['content']
   if prompt.startswith('请创建'):name='write_file';args=dict(path='sample.txt',text='Dunlin C 工具记录由 C 层持久保存。',expected_sha256='new')
   elif prompt.startswith('请运行'):name='shell';args=dict(command="printf 'TOOL_RECORDS_UI_OK\\n'")
   elif prompt.startswith('请读取'):name='read_file';args=dict(path='sample.txt')
   elif prompt.startswith('失败命令'):name='shell';args=dict(command="printf 'owned fixture failure\\n' >&2; exit 3")
   elif prompt.startswith('拒绝写入'):name='write_file';args=dict(path='denied.txt',text='must never publish',expected_sha256='new')
   else:name=None
   if name:calls=[dict(id='fixture-call',type='function',function=dict(name=name,arguments='  '+json.dumps(args,ensure_ascii=False)+'\n'))]
  message=dict(role='assistant',content=None,tool_calls=calls) if calls else dict(role='assistant',content='已根据实际工具结果记录本次操作。点击工具卡片可以进入独立的工具执行记录页。')
  body=json.dumps(dict(choices=[dict(index=0,message=message,finish_reason='tool_calls' if calls else 'stop')],usage=dict(prompt_tokens=94,completion_tokens=32,total_tokens=126))).encode();self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
db=lib.dc_open(str(state).encode());assert db
def req(op,**kw):return value(lib.dc_workspace_request(db,json.dumps(dict(op=op,**kw)).encode()))
assert req('projects.create',id='tool-records-project',name='工具记录验收项目',path=str(work),instructions='只操作独立验收目录。全部模型响应为本地 fixture。',model='tool-fixture-A',policy='ask',context=[])['ok']
for id,title in [('tool-records-main','工具执行记录验收'),('tool-records-failure','失败命令记录'),('tool-records-denied','拒绝授权记录')]:assert lib.dc_add_chat(db,id.encode(),title.encode(),str(work).encode())
endpoint=f'http://127.0.0.1:{server.server_port}/v1'
def run(chat,prompt,model='A'):
 cfg=dict(base_url=endpoint,model='fixture-'+model,protocol='chat',stream=False,desktop_profile_id='tool-fixture-'+model,desktop_tools='ask')
 return value(lib.dc_run(db,chat.encode(),json.dumps(cfg).encode(),prompt.encode(),C.byref(hooks)))
for prompt in ['请创建 sample.txt。','请运行本地验收命令。']:assert run('tool-records-main',prompt)['status']=='completed'
assert run('tool-records-main','请读取 sample.txt，核对切换模型后的上下文。','B')['status']=='completed'
assert run('tool-records-failure','失败命令：只返回本地非零退出码。')['status']=='failed'
allow=False;assert run('tool-records-denied','拒绝写入：验证不产生文件。')['status']=='denied';assert not (work/'denied.txt').exists()
assert req('chats.update',id='tool-records-main',field='model',value='tool-fixture-B')['ok']
profiles=[dict(id='tool-fixture-'+m,name='界面验收模型 '+m+'（离线 fixture）',endpoint=endpoint,model='fixture-'+m,protocolName='chat',maxOutput=1024) for m in ['A','B']]
(state/'settings.json').write_text(json.dumps(dict(profiles=profiles,profileID='tool-fixture-B',defaultProfileID='tool-fixture-B',theme='light',projects=[],showReasoning=False),ensure_ascii=False));(state/'settings.json').chmod(0o600)
records=req('tools.list',chat='tool-records-main')['data']['items'];assert len(records)==3 and all(r['stage']=='committed' for r in records)
assert any(m.get('role')=='user' and m.get('content')=='请创建 sample.txt。' for q in requests if q['model']=='fixture-B' for m in q['messages'])
lib.dc_close(db);server.shutdown();server.server_close()
report=dict(scope='Owned fixture HTTP, C authorized file/process effects and two model identities; not real-model quality',state=str(state),project=str(work),requests=len(requests),library_sha256=hashlib.sha256(library.read_bytes()).hexdigest(),records=[dict(id=r['id'],name=r['name'],run=r['run']) for r in records]);(root/'reports/v51-completion-20261107/tool-records-ui-fixture.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n');print('Prepared isolated C file/read/command, failure, denial and two-model history; fixture service now stopped.')
