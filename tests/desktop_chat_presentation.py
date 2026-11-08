#!/usr/bin/env python3
"""C chat projection/live durability with fixed loopback model data; no live account."""
import desktop_fixture_sqlite as fixture_sqlite
import ctypes as C, hashlib, http.server, importlib.util, json, pathlib, sqlite3, sys, tempfile, threading
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_messages',[P,S],P),('dc_read_view',[S,S],P),('dc_run_with_secret',[P,S,S,S,P,S],P)]:
 fn=getattr(lib,name);fn.argtypes=args;fn.restype=result
def enc(v):return json.dumps(v,ensure_ascii=False).encode()
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
def read(root,**kw):return value(lib.dc_read_view(str(root).encode(),enc(dict(op='chat.messages',**kw))))
with tempfile.TemporaryDirectory(prefix='dunlin-chat-projection-') as tmp:
 root=pathlib.Path(tmp);root.chmod(0o700);db=lib.dc_open(str(root).encode());assert db;assert lib.dc_add_chat(db,b'projection',b'Projection',str(root).encode())
 messages=[dict(role='user',content='Original input'),dict(role='assistant',content='Original answer',reasoning_content='Public plan')]
 with fixture_sqlite.connect(root/'conversations.sqlite') as sql:sql.execute("UPDATE chats SET state=? WHERE id='projection'",(json.dumps(dict(messages=messages)),))
 rows=value(lib.dc_messages(db,b'projection'));assert rows[1].get('reasoning')=='Public plan',('public reasoning missing from native chat DTO',rows)
 assert rows[1]['content']=='Original answer' and rows[1]['sourceHash']==hashlib.sha256(b'Original answer').hexdigest()
 assert read(root,chat='projection',through=0)['data']==rows[:1]
 for bound in [-1,True,'1',1.5,None,2**63]:assert not read(root,chat='projection',through=bound)['ok'],bound
 assert not read(root,chat='projection\x00other')['ok']
 assert read(root,chat='projection',through=1)['data']==rows
 assert read(root,chat='projection',through=100)['data']==rows
 mixed=[dict(role='assistant',content=None,reasoning_content='Public only'),dict(role='assistant',content=None,tool_calls=[]),dict(role='assistant',content='Valid answer',reasoning_content=42)]
 with fixture_sqlite.connect(root/'conversations.sqlite') as sql:sql.execute("UPDATE chats SET state=? WHERE id='projection'",(json.dumps(dict(messages=mixed)),))
 projected=read(root,chat='projection')['data'];assert [m['sequence'] for m in projected]==[0,2] and projected[0]['content']=='' and projected[0]['reasoning']=='Public only'
 assert projected[1]['reasoningUnavailable'] and 'reasoning' not in projected[1] and projected[1]['content']=='Valid answer'
 lib.dc_close(db)
 print('PASS original display/source hash plus public reasoning; typed canonical prefix cursor')
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
spec=importlib.util.spec_from_file_location('protocol_fixtures',pathlib.Path(__file__).with_name('protocol_e2e.py'));fixtures=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixtures)
requests=[];errors=[];events=[];commits=[];protocol='chat';stream=False;stage=0;current_root=None;current_chat=None
block_final=False;blocked=threading.Event();release=threading.Event()
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  global stage
  try:
   request=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append((protocol,request));selected_stage=1 if stage==0 else 2;stage+=1
   if block_final and selected_stage==2:blocked.set();assert release.wait(10)
   if protocol=='chat':
    message=dict(role='assistant',content=None if selected_stage==1 else fixtures.TEXT,reasoning_content=fixtures.THINKING)
    if selected_stage==1:message['tool_calls']=[dict(id='read-fixture',type='function',function=dict(name='read_file',arguments='{"path":"observed.txt"}'))]
    final=dict(choices=[dict(index=0,message=message,finish_reason='tool_calls' if selected_stage==1 else 'stop')])
    if stream:
     delta=dict(message);delta.pop('role',None)
     # Chat streamed tool arrays carry an index; this is real protocol framing.
     if 'tool_calls' in delta:delta['tool_calls'][0]['index']=0
     body=b'data: '+enc(dict(choices=[dict(index=0,delta=delta,finish_reason=None)]))+b'\n\ndata: '+enc(dict(choices=[dict(index=0,delta={},finish_reason=final['choices'][0]['finish_reason'])]))+b'\n\ndata: [DONE]\n\n'
    else:body=enc(final)
   else:body=fixtures.response_payload(protocol,selected_stage,stream)
   self.send_response(200);self.send_header('Content-Type','text/event-stream' if stream and protocol!='ollama' else 'application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
  except BaseException as e:errors.append(repr(e));self.close_connection=True
@EV
def event(_,name,text):
 try:
  name=name.decode();text=text.decode();events.append((name,text))
  if name=='chat.commit':
   notice=json.loads(text);assert set(notice)=={'chat','run','through','settlesModel'};assert notice['chat']==current_chat and isinstance(notice['through'],int) and notice['through']>=0
   reply=read(current_root,chat=current_chat,through=notice['through']);assert reply['ok'],reply;rows=reply['data'];assert all(m['sequence']<=notice['through'] for m in rows)
   with fixture_sqlite.connect(current_root/'conversations.sqlite') as sql:saved=json.loads(sql.execute('SELECT state FROM chats WHERE id=?',(current_chat,)).fetchone()[0])
   assert len(saved['messages'])>notice['through'];assert notice['settlesModel']==(saved['messages'][notice['through']]['role']=='assistant')
   commits.append((notice,rows))
 except BaseException as e:errors.append(repr(e))
@AP
def approval(*_):errors.append('unexpected approval in fixed read-only fixture');return 0
@CA
def cancelled(_):return 0
hooks=Hooks(None,event,approval,cancelled)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
try:
 with tempfile.TemporaryDirectory(prefix='dunlin-chat-wire-') as tmp:
  root=pathlib.Path(tmp);root.chmod(0o700);current_root=root;work=root/'project';work.mkdir();(work/'observed.txt').write_text(fixtures.TEXT);db=lib.dc_open(str(root).encode());assert db
  def run(config,prompt):return value(lib.dc_run_with_secret(db,current_chat.encode(),enc(config),prompt.encode(),C.byref(hooks),b''))
  for protocol in ['chat','responses','anthropic','ollama','gemini']:
   for stream in [False,True]:
    current_chat=protocol+('-stream' if stream else '-json');assert lib.dc_add_chat(db,current_chat.encode(),current_chat.encode(),str(work).encode());stage=0;events.clear();commits.clear();requests.clear()
    config=dict(base_url=f'http://127.0.0.1:{server.server_port}/'+('api' if protocol=='ollama' else 'v1'),model='fixture',protocol=protocol,stream=stream,max_tokens=256,show_reasoning=True,desktop_tools='readOnly')
    result=run(config,'Read the owned fixture');assert result['status']=='completed',(protocol,stream,result,errors)
    rows=read(root,chat=current_chat)['data'];reasoning_rows=[m for m in rows if m['role']=='assistant'];assert len(reasoning_rows)==2,(protocol,rows);assert all(m.get('modelName')=='fixture' for m in reasoning_rows),(protocol,rows)
    assert all(m.get('reasoning')==fixtures.THINKING for m in reasoning_rows),(protocol,rows)
    assert all(m['reasoningKind']==('summary' if protocol=='responses' else 'reasoning') for m in reasoning_rows)
    assert len([m for m in rows if m['role']=='tool'])==1 and fixtures.TEXT in rows[-1]['content'];assert fixtures.OPAQUE not in json.dumps(rows)
    assert len([n for n,_ in commits if n['settlesModel']])==2,(protocol,commits)
    assert any(name in ['reasoning.delta','reasoning.summary'] and text==fixtures.THINKING for name,text in events),(protocol,events)
    assert all(a[0]['through']<b[0]['through'] for a,b in zip(commits,commits[1:]));first=list(rows)
    with fixture_sqlite.connect(root/'conversations.sqlite') as sql:canonical_before=json.loads(sql.execute('SELECT state FROM chats WHERE id=?',(current_chat,)).fetchone()[0])['messages']
    if protocol in ['responses','anthropic','gemini']:assert fixtures.OPAQUE in json.dumps(canonical_before)
    with fixture_sqlite.connect(root/'conversations.sqlite') as sql:assert not any(fixtures.THINKING in row[0] for row in sql.execute('SELECT text FROM index_jobs WHERE chat=?',(current_chat,)))
    # Native display visibility is local UI state; its transport show_reasoning
    # stays true. Verify exact-profile continuation before separately exercising
    # the legacy C callback switch (which is a different frozen configuration).
    result=run(config,'Same frozen model configuration, local display may be hidden');assert result['status']=='completed',(protocol,result)
    if protocol in ['responses','anthropic','gemini']:assert fixtures.OPAQUE in json.dumps(requests[-1][1]),(protocol,requests[-1])
    if protocol=='chat':assert any(m.get('reasoning_content')==fixtures.THINKING for m in requests[-1][1]['messages'])
    events.clear();commits.clear();result=run(dict(config,show_reasoning=False),'Preserve context while reasoning display is hidden');assert result['status']=='completed',(protocol,result)
    assert not any(name.startswith('reasoning.') for name,_ in events);assert read(root,chat=current_chat)['data'][:len(first)]==first
    # Route change retains canonical original capsules but sends only portable history.
    old_protocol=protocol;old_stream=stream;protocol='chat';stream=False;stage=1;result=run(dict(config,protocol='chat',model='fixture-other',base_url=f'http://127.0.0.1:{server.server_port}/v1',stream=False,show_reasoning=False),'Switch model and retain original context');protocol=old_protocol;stream=old_stream
    assert result['status']=='completed',(protocol,result);assert all('reasoning_content' not in m for m in requests[-1][1]['messages']),('provider-specific reasoning field crossed to another model',requests[-1]);assert fixtures.OPAQUE not in json.dumps(requests[-1][1]);assert 'Read the owned fixture' in json.dumps(requests[-1][1]);assert read(root,chat=current_chat)['data'][:len(first)]==first;assert read(root,chat=current_chat)['data'][-1].get('modelName')=='fixture-other'
    with fixture_sqlite.connect(root/'conversations.sqlite') as sql:after=json.loads(sql.execute('SELECT state FROM chats WHERE id=?',(current_chat,)).fetchone()[0])['messages']
    assert after[:len(canonical_before)]==canonical_before;assert not errors,errors
    print('PASS actual '+protocol+(' stream' if stream else ' JSON')+': public reasoning, live durable tool/answer commits, hidden reasoning toggle, private continuation and portable model switch')
  protocol='chat';stream=False;stage=0;events.clear();commits.clear();requests.clear();current_chat='live-prefix';assert lib.dc_add_chat(db,current_chat.encode(),b'Live prefix',str(work).encode());block_final=True;blocked.clear();release.clear();answers=[]
  config=dict(base_url=f'http://127.0.0.1:{server.server_port}/v1',model='fixture',protocol='chat',stream=False,show_reasoning=True,desktop_tools='readOnly')
  worker=threading.Thread(target=lambda:answers.append(run(config,'Read while final answer is blocked')));worker.start()
  try:
   assert blocked.wait(10);visible=read(root,chat=current_chat)['data'];assert any(m['role']=='tool' for m in visible) and visible[-1]['role']=='tool';assert len(commits)>=3
   notice=commits[0][0];assert read(root,chat=current_chat,through=notice['through'])['data']==commits[0][1]
  finally:release.set();worker.join(12);block_final=False
  assert not worker.is_alive() and answers[0]['status']=='completed';assert read(root,chat=current_chat,through=notice['through'])['data']==commits[0][1]
  print('PASS WAL prefix visible while model/writer run is blocked; old prefix remains identical after later commits')
  current_chat='commit-fault';stage=1;commits.clear();assert lib.dc_add_chat(db,current_chat.encode(),b'Fault',str(work).encode())
  with fixture_sqlite.connect(root/'conversations.sqlite') as sql:sql.execute("CREATE TRIGGER reject_answer BEFORE UPDATE OF state ON chats WHEN NEW.id='commit-fault' AND json_array_length(json_extract(NEW.state,'$.messages'))>1 BEGIN SELECT RAISE(ABORT,'owned final commit failure');END")
  result=run(config,'Do not claim a failed answer was committed');assert result['status']!='completed' and commits and not any(n['settlesModel'] for n,_ in commits),(result,commits)
  assert not any(m['role']=='assistant' for m in read(root,chat=current_chat)['data']);assert not errors,errors;lib.dc_close(db)
  print('PASS failed SQL answer commit emits no successful answer notification and no visible fabricated answer')
finally:server.shutdown();server.server_close();thread.join(2)
