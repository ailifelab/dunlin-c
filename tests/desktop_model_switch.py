#!/usr/bin/env python3
"""C model route review/apply; deterministic owned HTTP/file fixtures only."""
import desktop_fixture_sqlite as fixture_sqlite
import ctypes as C,hashlib,http.server,json,pathlib,sqlite3,sys,tempfile,threading
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_workspace_request',[P,S],P),('dc_read_view',[S,S],P),('dc_messages',[P,S],P),('dc_run_with_secret',[P,S,S,S,P,S],P)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=result
def enc(v):return json.dumps(v,ensure_ascii=False).encode()
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
approvals=[];requests=[];errors=[]
@EV
def event(*_):pass
@AP
def approve(_,name,raw):
 try:
  args=json.loads(raw);assert name==b'write_file' and args==dict(path='greeting.txt',text='Actual switch fixture effect',expected_sha256='new');approvals.append((name,raw));return 1
 except BaseException as e:errors.append(repr(e));return 0
@CA
def cancelled(_):return 0
hooks=Hooks(None,event,approve,cancelled)
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  try:
   request=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(request)
   if self.path.endswith('/responses'):reply=dict(id='owned',status='completed',output=[dict(id='answer',type='message',role='assistant',status='completed',content=[dict(type='output_text',text='Continued original file context.',annotations=[])])])
   else:
    messages=request['messages'];last_user=max(i for i,m in enumerate(messages) if m['role']=='user');tail=messages[last_user+1:];calls=[]
    if not tail:calls=[dict(id='write',type='function',function=dict(name='write_file',arguments=json.dumps(dict(path='greeting.txt',text='Actual switch fixture effect',expected_sha256='new'))))]
    elif len([m for m in tail if m['role']=='tool'])==1:calls=[dict(id='read',type='function',function=dict(name='read_file',arguments='{"path":"greeting.txt"}'))]
    message=dict(role='assistant',content=None,tool_calls=calls) if calls else dict(role='assistant',content='Actual source retained.')
    reply=dict(choices=[dict(index=0,message=message,finish_reason='tool_calls' if calls else 'stop')])
   body=enc(reply);self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
  except BaseException as e:errors.append(repr(e))
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
try:
 with tempfile.TemporaryDirectory(prefix='dunlin-model-switch-') as tmp:
  root=pathlib.Path(tmp);state=root/'state';state.mkdir(mode=0o700);work=root/'project';work.mkdir();db=lib.dc_open(str(state).encode());assert db
  def req(op,**kw):return value(lib.dc_workspace_request(db,enc(dict(op=op,**kw))))
  def ok(op,**kw):
   r=req(op,**kw);assert r['ok'],r;return r.get('data')
  def read(op,**kw):return value(lib.dc_read_view(str(state).encode(),enc(dict(op=op,**kw))))
  model=dict(id='a',name='First model',endpoint=f'http://127.0.0.1:{server.server_port}/v1',model='fixture-a',protocolName='chat',maxOutput=256,authentication='none',contextLimit=8192)
  second=dict(model,id='b',name='Second model',protocolName='responses',model='fixture-b');registry=ok('config.save',revision=0,models=[model,second],providers=[])
  ok('projects.create',id='project',name='Fixture project',path=str(work),instructions='',model='a',policy='ask',context=[])
  assert lib.dc_add_chat(db,b'chat',b'Actual conversation',str(work).encode());ok('chats.update',id='chat',field='model',value='a');ok('chats.update',id='chat',field='draft',value='Keep my unsent draft')
  def preview(target='b',chat='chat',revision=None):return req('chats.model.preview',chat=chat,target=target,revision=registry['revision'] if revision is None else revision)
  def apply(plan,**kw):return req('chats.model.apply',**dict(dict(chat=plan['chat'],target=plan['targetID'],revision=plan['configRevision'],currentID=plan['currentID'],historyHash=plan['historyHash']),**kw))
  first=preview();assert first['ok'],('missing C model-switch review',first);assert first['data']['messageCount']==0 and first['data']['canSwitch']
  config=ok('config.model',id='a',revision=registry['revision']);config['stream']=False
  result=value(lib.dc_run_with_secret(db,b'chat',enc(config),b'Create and verify the owned greeting file',C.byref(hooks),b''));assert result['status']=='completed',(result,errors);assert not errors and len(approvals)==1
  original=(work/'greeting.txt').read_bytes();assert original==b'Actual switch fixture effect';rows=value(lib.dc_messages(db,b'chat'));assert all(m.get('modelName')=='fixture-a' for m in rows if m['role']=='assistant')
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:source=sql.execute("SELECT state FROM chats WHERE id='chat'").fetchone()[0]
  before_requests=len(requests);plan=preview()['data'];assert plan['messageCount']==6 and plan['toolResults']==2 and plan['fileReceipts']==1 and plan['contextKnown'] and plan['contextSelected']>0 and plan['canSwitch'] and plan['imageCount']==0,plan
  assert plan['currentID']=='a' and plan['targetID']=='b' and plan['historyHash']==hashlib.sha256(source.encode()).hexdigest() and plan['contextFit']=='unmeasured'
  assert read('chats.model.preview',chat='chat',target='b',revision=registry['revision'])['data']==plan
  assert not read('chats.model.apply',chat='chat',target='b',revision=registry['revision'],currentID='a',historyHash=plan['historyHash'])['ok']
  assert len(requests)==before_requests and ok('chats.metadata',id='chat')['model']=='a' # preview/cancel is inert
  result=apply(plan);assert result['ok'],result
  assert ok('chats.metadata',id='chat')==dict(draft='Keep my unsent draft',model='b');assert value(lib.dc_messages(db,b'chat'))==rows
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:assert sql.execute("SELECT state FROM chats WHERE id='chat'").fetchone()[0]==source
  assert len(requests)==before_requests and (work/'greeting.txt').read_bytes()==original and len(approvals)==1
  assert apply(plan)['error']=='model_switch_history_changed'
  config=ok('config.model',id='b',revision=registry['revision']);config['stream']=False
  result=value(lib.dc_run_with_secret(db,b'chat',enc(config),b'Continue with the retained history',C.byref(hooks),b''));assert result['status']=='completed',result
  assert 'Create and verify the owned greeting file' in json.dumps(requests[-1]) and 'Actual switch fixture effect' in json.dumps(requests[-1]);assert len(approvals)==1 and (work/'greeting.txt').read_bytes()==original
  assert value(lib.dc_messages(db,b'chat'))[:len(rows)]==rows and value(lib.dc_messages(db,b'chat'))[-1]['modelName']=='fixture-b'
  print('PASS exact review counts after real fixture file/read effects; inert preview/cancel, read-only query, atomic preferred-route apply, unchanged checkpoint/draft/receipts/tools and actual next-protocol history')
  plan=preview(target='a')['data'];registry=ok('config.save',revision=registry['revision'],models=[model,dict(second,name='Renamed second')],providers=[]);assert apply(plan)['error']=='model_switch_configuration_changed'
  plan=preview(target='a')['data']
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:
   saved=sql.execute("SELECT state FROM chats WHERE id='chat'").fetchone()[0];changed=json.loads(saved);changed['detail']='Owned changed settlement';sql.execute("UPDATE chats SET state=? WHERE id='chat'",(json.dumps(changed),))
  assert apply(plan)['error']=='model_switch_history_changed';assert ok('chats.metadata',id='chat')['model']=='b'
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:sql.execute("UPDATE chats SET state=? WHERE id='chat'",(saved,))
  plan=preview(target='a')['data'];ok('chats.update',id='chat',field='model',value='other-ui-choice');assert apply(plan)['error']=='model_switch_history_changed';ok('chats.update',id='chat',field='model',value='b')
  ok('chats.update',id='chat',field='model',value='legacy route alias')
  legacy=preview(target='a')['data'];assert legacy['currentID']=='legacy route alias' and apply(legacy)['ok'];ok('chats.update',id='chat',field='model',value='b')
  print('PASS stale registry revision, canonical settlement and another preferred selection reject without changing source or executing anything')
  for altered,expected in [(dict(json.loads(saved),active=True),'model_switch_active_checkpoint'),(dict(json.loads(saved),pending={}), 'model_switch_active_checkpoint'),(dict(json.loads(saved),messages={}), 'model_switch_history_invalid'),(dict(json.loads(saved),active=None),'model_switch_history_invalid'),(dict(json.loads(saved),version=999),'model_switch_history_invalid')]:
   with fixture_sqlite.connect(state/'conversations.sqlite') as sql:sql.execute("UPDATE chats SET state=? WHERE id='chat'",(json.dumps(altered),))
   assert preview(target='a')['error']==expected
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:sql.execute("UPDATE chats SET state=? WHERE id='chat'",('broken source',))
  assert preview(target='a')['error']=='model_switch_history_invalid'
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:sql.execute("UPDATE chats SET state=? WHERE id='chat'",(saved,))
  for bad in [-1,True,'1',1.0,2**63]:assert not preview(revision=bad)['ok']
  assert not req('chats.model.preview',chat='chat\x00other',target='a',revision=registry['revision'])['ok'];assert preview(target='missing')['error']=='model_switch_model_missing'
  plan=preview(target='a')['data']
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:sql.execute("CREATE TRIGGER reject_route BEFORE UPDATE OF model ON chat_properties WHEN NEW.chat='chat' BEGIN SELECT RAISE(ABORT,'owned route publication failure');END")
  assert apply(plan)['error']=='model_switch_storage_error';assert ok('chats.metadata',id='chat')['model']=='b'
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:sql.execute('DROP TRIGGER reject_route');assert sql.execute("SELECT state FROM chats WHERE id='chat'").fetchone()[0]==saved
  print('PASS active/unknown/corrupt/future-format/type guards, exact IDs and failed publication rollback')
  assert lib.dc_add_chat(db,b'image',b'Image history',str(work).encode());image_state=dict(version=1,rounds=0,active=False,messages=[dict(role='user',content=[dict(type='text',text='Original image prompt'),dict(type='image_url',image_url=dict(url='data:image/png;base64,fixture-only'))])])
  with fixture_sqlite.connect(state/'conversations.sqlite') as sql:sql.execute("UPDATE chats SET state=? WHERE id='image'",(json.dumps(image_state),))
  plan=preview(target='a',chat='image')['data'];assert not plan['canSwitch'] and plan['reason']=='model_switch_image_capability_unconfirmed' and plan['imageCount']==1;assert apply(plan)['error']==plan['reason']
  model=dict(model,inputModalities=['text','image']);registry=ok('config.save',revision=registry['revision'],models=[model,second],providers=[])
  plan=preview(target='a',chat='image')['data'];assert plan['canSwitch'] and apply(plan)['ok']
  ollama=dict(second,id='ollama',protocolName='ollama',inputModalities=['text','image']);registry=ok('config.save',revision=registry['revision'],models=[model,second,ollama],providers=[]);plan=preview(target='ollama',chat='image')['data'];assert not plan['canSwitch'] and plan['reason']=='model_switch_image_protocol_unsupported' and apply(plan)['error']==plan['reason']
  assert not errors;lib.dc_close(db)
  print('PASS image history requires explicit supported model/protocol capability; canonical image is never silently dropped (image fixtures do not claim actual decoding/vision)')
finally:server.shutdown();server.server_close();worker.join(2)
