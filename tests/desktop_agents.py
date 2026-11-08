#!/usr/bin/env python3
"""Actual C task execution, canonical inbox commit and tool effects against owned HTTP fixture."""
import ctypes as C, pathlib, json, sys, tempfile, threading, http.server, hashlib
import desktop_fixture_sqlite as fixture_sqlite
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for name,args,out in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_workspace_request',[P,S],P),('dc_agent_execute',[P,S,S,S,P],P),('dc_run_message_with_secret',[P,S,S,S,P,S,S,C.POINTER(C.c_int)],P),('dc_messages',[P,S],P),('dc_delete_chat',[P,S],C.c_int)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=out
def enc(x):return json.dumps(x,ensure_ascii=False).encode()
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
requests=[];approvals=[];approval_origins=[];latest_origin=None;allow=True;stop=False
@EV
def event(_,name,data):
 global latest_origin
 if name==b"agent.approval":latest_origin=json.loads(data)
@AP
def approve(_,tool,args):
 approvals.append((tool.decode(),json.loads(args)));approval_origins.append(latest_origin);return int(allow)
@CA
def cancelled(_):return int(stop)
hooks=Hooks(None,event,approve,cancelled)
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  r=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(r);m=r['messages'];last=m[-1]
  if last['role']=='user' and last['content']=='send peer message':
   assert {'list_project_agents','send_agent_message'} <= {t['function']['name'] for t in r['tools']}
   message=dict(role='assistant',content=None,tool_calls=[dict(id='peer-owned',type='function',function=dict(name='send_agent_message',arguments=json.dumps(dict(receiver='agent-first',id='model-peer',text='请保留文件，不要覆盖。'))))]);finish='tool_calls'
  elif last['role']=='user' and last['content']=='recursive delegation':
   message=dict(role='assistant',content=None,tool_calls=[dict(id='recursive-owned',type='function',function=dict(name='delegate_agent',arguments=json.dumps(dict(task='recursive delegation'))))]);finish='tool_calls'
  elif last['role']=='user' and last['content']=='delegate child write':
   assert any(t['function']['name']=='delegate_agent' for t in r['tools'])
   message=dict(role='assistant',content=None,tool_calls=[dict(id='delegate-owned',type='function',function=dict(name='delegate_agent',arguments=json.dumps(dict(task='write from delegated child'))))]);finish='tool_calls'
  elif last['role']=='user' and last['content'].startswith('write'):
   message=dict(role='assistant',content=None,tool_calls=[dict(id='write-owned',type='function',function=dict(name='write_file',arguments=json.dumps(dict(path='agent.txt',text='Actual Agent tool effect',expected_sha256='new'))))]);finish='tool_calls'
  else:message=dict(role='assistant',content='Task finished with preserved source.');finish='stop'
  body=enc(dict(choices=[dict(index=0,message=message,finish_reason=finish)]));self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
try:
 with tempfile.TemporaryDirectory(prefix='dunlin-agents-') as tmp:
  root=pathlib.Path(tmp);state=root/'state';state.mkdir(mode=0o700);work=root/'work';work.mkdir();other=root/'other';other.mkdir();db=lib.dc_open(str(state).encode());assert db
  def req(op,**kw):return value(lib.dc_workspace_request(db,enc(dict(op=op,**kw))))
  def ok(op,**kw):
   r=req(op,**kw);assert r['ok'],r;return r.get('data')
  model=dict(id='a',name='Agent A',endpoint=f'http://127.0.0.1:{server.server_port}/v1',model='owned-a',protocolName='chat',maxOutput=256,authentication='none')
  revision=ok('config.save',revision=0,models=[model,dict(model,id='b',name='Agent B',model='owned-b')],providers=[])['revision']
  for project,path in [('project',work),('other',other)]:ok('projects.create',id=project,name=project,path=str(path),instructions='',model='a',policy='ask',context=[])
  def create(id,task='hello',model='a',project='project',chat='',parent=''):
   ok('agents.create',id=id,project=project,model=model,task=task,revision=revision,chat=chat,parent=parent)
  def run(id,model='a'):
   cfg=ok('config.model',id=model,revision=revision);cfg['stream']=False
   return value(lib.dc_agent_execute(db,id.encode(),enc(cfg),b'',C.byref(hooks)))
  def jobs():return {x['id']:x for x in ok('agents.list')}
  create('first','write');create('second','Use the shared message',model='b');create('foreign',project='other')
  assert ok('chats.metadata',id='agent-second')['model']=='b'
  assert not req('agents.join',project='project',chat='agent-foreign')['ok']
  ok('agents.join',project='project',chat='agent-first');assert [m['id'] for m in ok('agents.members',project='project')]==['agent-first']
  assert ok('agents.members',project='other')==[]
  assert not req('agents.send',project='project',sender='agent-first',receiver='agent-foreign',id='cross',text='must not escape')['ok']
  receipt=ok('agents.send',project='project',sender='agent-first',receiver='agent-second',id='msg',text='不要覆盖源文件；这是协作资料。')
  assert ok('agents.messages',project='other')==[]
  shared=ok('agents.messages',project='project');assert len(shared)==1 and not shared[0]['delivered'] and shared[0]['text']=='不要覆盖源文件；这是协作资料。'
  assert ok('agents.send',project='project',sender='agent-first',receiver='agent-second',id='msg',text='不要覆盖源文件；这是协作资料。')==receipt
  assert not req('agents.send',project='project',sender='agent-first',receiver='agent-second',id='msg',text='different')['ok']
  count=len(requests);assert run('first','b')['status']=='failed';assert len(requests)==count and jobs()['first']['state']=='queued'
  assert run('first')['status']=='completed';assert (work/'agent.txt').read_text()=='Actual Agent tool effect';assert len(approvals)==1 and approvals[0][0]=='write_file';assert jobs()['first']['state']=='succeeded'
  count=len(requests);assert run('first')['status']=='failed' and len(requests)==count
  assert run('second','b')['status']=='completed';assert requests[-1]['model']=='owned-b' and '不要覆盖源文件' in requests[-1]['messages'][-1]['content']
  assert ok('agents.messages',project='project')[0]['delivered']
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:
   seq,digest=sql.execute('SELECT ack_sequence,ack_hash FROM agent_mailbox_messages WHERE message_id=?',('msg',)).fetchone();source=json.loads(sql.execute('SELECT state FROM chats WHERE id=?',('agent-second',)).fetchone()[0])['messages'][seq];text=source.get('_dunlin_display',source['content']);assert hashlib.sha256(text.encode()).hexdigest()==digest
  ok('agents.send',project='project',sender='agent-first',receiver='agent-second',id='regular',text='Normal chat also receives peer context')
  cfg=ok('config.model',id='b',revision=revision);cfg['stream']=False;committed=C.c_int()
  ok('chats.update',id='agent-second',field='draft',value='normal chat turn')
  response=value(lib.dc_run_message_with_secret(db,b'agent-second',enc(cfg),b'normal chat turn',C.byref(hooks),b'',None,C.byref(committed)))
  assert response['status']=='completed' and committed.value==1
  assert any('Normal chat also receives peer context' in (m.get('content') or '') for m in requests[-1]['messages'])
  assert ok('chats.metadata',id='agent-second')['draft']==''
  assert all(m['delivered'] for m in ok('agents.messages',project='project'))
  create('follow','Continue original chat',model='a',chat='agent-second');assert run('follow')['status']=='completed';assert any('不要覆盖源文件' in (m.get('content') or '') for m in requests[-1]['messages']);assert requests[-1]['model']=='owned-a'
  create('cancel');ok('agents.cancel',id='cancel');count=len(requests);assert run('cancel')['status']=='failed' and len(requests)==count
  create('deny','write');(work/'agent.txt').unlink();allow=False;assert run('deny')['status']!='completed';assert not (work/'agent.txt').exists();allow=True
  create('delegating','delegate child write');prior_approvals=len(approvals);assert run('delegating')['status']=='completed'
  assert (work/'agent.txt').read_text()=='Actual Agent tool effect'
  children=[j for j in jobs().values() if j['parent']=='delegating'];assert len(children)==1 and children[0]['state']=='succeeded'
  assert [a[0] for a in approvals[prior_approvals:]]==['delegate_agent','write_file']
  origins=approval_origins[prior_approvals:];assert origins[0] and origins[0]['job']=='delegating',origins
  assert origins[1]['job']!=origins[0]['job'] and origins[1]['chat']!=origins[0]['chat'] and origins[1]['task']=='write from delegated child',origins
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:
   parent_source=json.loads(sql.execute("SELECT state FROM chats WHERE id='agent-delegating'").fetchone()[0])['messages']
   assert any(m.get('role')=='tool' and children[0]['id'] in m.get('content','') for m in parent_source)
   assert not any(m.get('role')=='user' and m.get('content')=='write from delegated child' for m in parent_source)
  create('delegate-denied','delegate child write');allow=False;count=len(requests);assert run('delegate-denied')['status']!='completed';assert len(requests)==count+1;assert not any(j['parent']=='delegate-denied' for j in jobs().values());allow=True
  create('recursive','recursive delegation');before_jobs=set(jobs());prior_approvals=len(approvals);assert run('recursive')['status']=='completed'
  recursive_children=[j for j in jobs().values() if j['id'] not in before_jobs];assert len(recursive_children)==8
  assert len(approvals)-prior_approvals==8 and all(a[0]=='delegate_agent' for a in approvals[prior_approvals:])
  assert not any(j['state'] in ('running','waiting') for j in jobs().values())
  create('peer','send peer message');before_peer=len(approvals)
  assert run('peer')['status']=='completed'
  assert approvals[before_peer][0]=='send_agent_message'
  assert any(m['sender']=='agent-peer' and m['receiver']=='agent-first' and m['text']=='请保留文件，不要覆盖。' and not m['delivered'] for m in ok('agents.messages',project='project'))
  create('rollback');ok('agents.send',project='project',sender='agent-first',receiver='agent-rollback',id='rollback-mail',text='Must be atomic')
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:sql.execute("CREATE TRIGGER fail_agent_ack BEFORE UPDATE OF ack_sequence ON agent_mailbox_messages BEGIN SELECT RAISE(ABORT,'owned ack failure'); END")
  count=len(requests);assert run('rollback')['status']!='completed';assert len(requests)==count
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:
   assert sql.execute("SELECT ack_sequence FROM agent_mailbox_messages WHERE message_id='rollback-mail'").fetchone()[0] is None
   raw=sql.execute("SELECT state FROM chats WHERE id='agent-rollback'").fetchone()[0];assert raw is None or not json.loads(raw).get('messages')
   sql.execute('DROP TRIGGER fail_agent_ack')
  create('restart');
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:sql.execute("UPDATE agent_jobs SET state='running',generation=123 WHERE id='restart'")
  count=len(requests);lib.dc_close(db);db=lib.dc_open(str(state).encode());assert db and len(requests)==count and jobs()['restart']['state']=='interrupted';assert run('restart')['status']=='failed' and len(requests)==count
  assert lib.dc_delete_chat(db,b'agent-second');assert 'second' not in jobs() and 'follow' not in jobs()
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:assert sql.execute("SELECT count(*) FROM agent_mailbox_messages WHERE receiver='agent-second'").fetchone()[0]==0
  lib.dc_close(db)
  print('PASS C scheduler executes real model loop; approved/denied actual file effects; context retained across task models; scoped deduplicated inbox; atomic source/ack rollback; cancellation, no replay, restart and deletion cleanup')
finally:server.shutdown();server.server_close()
