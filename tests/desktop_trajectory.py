#!/usr/bin/env python3
"""Local C observed ledger against real HTTP/tool fixtures, not model-quality evidence."""
import ctypes as C,json,sys,pathlib,tempfile,threading,http.server,sqlite3,time,subprocess,os
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_delete_chat',[P,S],C.c_int),('dc_messages',[P,S],P),('dc_run_with_secret',[P,S,S,S,P,S],P),('dc_workspace_request',[P,S],P),('dc_read_trajectory',[S,S],P),('dl_trajectory_open',[S],P),('dl_trajectory_close',[P],None),('dl_trajectory_begin',[P,S,S,S,S,C.c_longlong,S],P),('dl_trajectory_event',[P,S,S,C.c_size_t],None),('dl_trajectory_finish',[P,S,C.c_longlong],None),('dl_trajectory_request',[P,P],P)]:
 f=getattr(lib,n);f.argtypes=a;f.restype=r
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
def val(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
requests=[];entered=threading.Event();release=threading.Event();approvals=[];retry_count=0;cancel_entered=threading.Event();cancel_release=threading.Event();stop=threading.Event()
@EV
def event(*_):pass
@AP
def approve(_,name,args):approvals.append((name.decode(),json.loads(args)));time.sleep(.015);return 1
@CA
def cancelled(_):return int(stop.is_set())
hooks=Hooks(None,event,approve,cancelled)
secret='fixture-model-key-DO-NOT-RETAIN';secret_bytes=secret.encode()
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  global retry_count
  q=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(q);last=q['messages'][-1];content=last.get('content') or ''
  if content=='wait for trace':entered.set();release.wait(8)
  if content=='cancel fixture':cancel_entered.set();cancel_release.wait(8)
  if content=='retry fixture' and retry_count==0:
   retry_count+=1;data=json.dumps({'error':{'code':'invalid_tool_call','message':'owned proposal rejection'}}).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data);return
  if content=='http failure':self.send_error(503);return
  if content=='stream fixture':
   chunks=[{'choices':[{'index':0,'delta':{'role':'assistant','reasoning_content':'PRIVATE_RAW_STREAM'},'finish_reason':None}]},
           {'choices':[{'index':0,'delta':{'content':'Stream '+secret[:12]},'finish_reason':None}]},
           {'choices':[{'index':0,'delta':{'content':secret[12:]+' 中文'},'finish_reason':None}]},
           {'choices':[{'index':0,'delta':{},'finish_reason':'stop'}]},
           {'choices':[],'usage':{'prompt_tokens':20,'completion_tokens':7,'total_tokens':27}}]
   data=b''.join(b'data: '+json.dumps(c,ensure_ascii=False).encode()+b'\n\n' for c in chunks)+b'data: [DONE]\n\n';self.send_response(200);self.send_header('Content-Type','text/event-stream');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data);return
  if content=='write fixture':msg={'role':'assistant','content':None,'tool_calls':[{'id':'trace-write','type':'function','function':{'name':'write_file','arguments':json.dumps({'path':'trace.txt','text':'trajectory effect','expected_sha256':'new'})}}]};finish='tool_calls'
  else:msg={'role':'assistant','content':'Observed '+secret+'; no invented completion.','reasoning_content':'PRIVATE_REASONING_NEVER_RETAIN'};finish='stop'
  body=json.dumps({'choices':[{'index':0,'message':msg,'finish_reason':finish}],'usage':{'prompt_tokens':20,'completion_tokens':7,'total_tokens':27}}).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers()
  try:self.wfile.write(body)
  except (BrokenPipeError,ConnectionResetError):pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='dunlin-trajectory-') as td:
 root=pathlib.Path(td);state=root/'state';state.mkdir(mode=0o700);work=root/'work';work.mkdir();db=lib.dc_open(str(state).encode());assert db;assert lib.dc_add_chat(db,b'chat',b'Trajectory',str(work).encode())
 def req(op,**kw):return val(lib.dc_workspace_request(db,json.dumps(dict(op=op,**kw)).encode()))
 def read(op,**kw):return val(lib.dc_read_trajectory(str(state).encode(),json.dumps(dict(op=op,**kw)).encode()))
 def run(text,stream=False):
  cfg=json.dumps({'base_url':f'http://127.0.0.1:{server.server_port}/v1','model':'trace-fixture','protocol':'chat','stream':stream,'desktop_tools':'ask','show_reasoning':True}).encode()
  return val(lib.dc_run_with_secret(db,b'chat',cfg,text.encode(),C.byref(hooks),secret.encode()))
 assert read('trace.segments',chat='chat')['data']['items']==[]
 assert run('write fixture')['status']=='completed';assert (work/'trace.txt').read_text()=='trajectory effect' and len(approvals)==1
 segment=read('trace.segments',chat='chat')['data']['items'][0];assert segment['status']=='completed' and segment['sourceStart']==0 and segment['sourceEnd']==4 and segment['duration']>=15,segment
 page=read('trace.events',chat='chat',segment=segment['id'])['data'];events=sorted(page['items'],key=lambda e:e['id']);kinds=[e['kind'] for e in events]
 assert kinds.count('model.request')==2 and kinds.count('model.response')==2 and kinds.count('model.request.end')==2,kinds
 assert kinds.index('approval.request')<kinds.index('approval.decision')<kinds.index('tool.start')<kinds.index('tool.post_execute')<kinds.index('tool.finalized')<kinds.index('turn.end'),kinds
 decision=next(e['body'] for e in events if e['kind']=='approval.decision');assert decision['allowed'] and decision['committed'] and decision['wait_ms']>=10
 assert all(e['body']['usage_present']==7 and e['body']['total_tokens']==27 for e in events if e['kind']=='model.metrics')
 exported=read('trace.export',chat='chat',segment=segment['id'])['data'];raw=json.dumps(exported);assert secret not in raw and 'PRIVATE_REASONING_NEVER_RETAIN' not in raw and '[REDACTED]' in raw
 assert len(read('trace.events',chat='chat',segment=segment['id'],kind='approval.')['data']['items'])==2
 assert read('trace.events',chat='chat',segment=segment['id'],query='trajectory effect')['data']['items']
 assert lib.dc_add_chat(db,b'other',b'Other',str(work).encode());assert not read('trace.events',chat='other',segment=segment['id'])['ok']
 print('PASS actual approved fixture write; model/approval/tool/commit boundaries, real usage, source ranges, local export redaction and chat isolation')
 result=[];thread=threading.Thread(target=lambda:result.append(run('wait for trace')));thread.start();assert entered.wait(5)
 current=read('trace.segments',chat='chat')['data']['items'][0];assert current['status']=='recording' and 'duration' not in current
 visible=read('trace.events',chat='chat',segment=current['id'])['data']['items'];assert any(e['kind']=='model.request' for e in visible)
 assert read('trace.segments',chat='chat')['data']['items'][0]['status']=='recording','reader must not mark active writer interrupted'
 release.set();thread.join(8);assert result[0]['status']=='completed'
 assert run('http failure')['status']=='failed';failure=read('trace.segments',chat='chat')['data']['items'][0];assert failure['status']=='failed'
 errors=read('trace.events',chat='chat',segment=failure['id'])['data']['items'];assert any(e['kind']=='model.request.end' and not e['body']['accepted'] for e in errors)
 assert run('stream fixture',True)['status']=='completed';streamed=read('trace.segments',chat='chat')['data']['items'][0];assert streamed['firstText']>=0
 stream_events=read('trace.events',chat='chat',segment=streamed['id'])['data']['items'];first=[e for e in stream_events if e['kind']=='model.first_visible_text'];assert len(first)==1 and first[0]['body']['latency_ms']>=0
 visible=[e for e in stream_events if e['kind']=='model.visible_text'];assert len(visible)==1 and '[REDACTED]' in visible[0]['body']['text'];assert secret not in json.dumps(stream_events) and 'PRIVATE_RAW_STREAM' not in json.dumps(stream_events)
 assert run('retry fixture')['status']=='completed';retried=read('trace.segments',chat='chat')['data']['items'][0];retry_events=read('trace.events',chat='chat',segment=retried['id'])['data']['items'];assert sum(e['kind']=='model.request' for e in retry_events)==2 and any(e['kind']=='model.proposal_retry' for e in retry_events);assert any(e['kind']=='model.request.end' and not e['body']['accepted'] for e in retry_events)
 cancelled_result=[];thread=threading.Thread(target=lambda:cancelled_result.append(run('cancel fixture')));thread.start();assert cancel_entered.wait(5);stop.set();thread.join(7);cancel_release.set();assert not thread.is_alive() and cancelled_result[0]['status']=='cancelled',cancelled_result;stop.clear()
 cancelled_segment=read('trace.segments',chat='chat')['data']['items'][0];assert cancelled_segment['status']=='cancelled';cancel_events=read('trace.events',chat='chat',segment=cancelled_segment['id'])['data']['items'];assert any(e['kind']=='model.request.end' and not e['body']['accepted'] for e in cancel_events)
 print('PASS actual streamed first-visible-text observation and split-key redaction; explicit proposal retry and in-flight cancellation retain distinct attempts/outcomes')
 print('PASS separate read-only WAL snapshot during active request and explicit HTTP failure without claiming tool/turn success')
 before=len(req('trace.segments',chat='chat')['data']['items']);assert req('trace.config',enabled=False)['ok'];assert run('unrecorded')['status']=='completed';assert len(req('trace.segments',chat='chat')['data']['items'])==before
 assert req('trace.config',enabled=True,capture=False)['ok'];assert run('timing only')['status']=='completed';timing=req('trace.segments',chat='chat')['data']['items'][0];e=req('trace.events',chat='chat',segment=timing['id'])['data']['items'];assert all('omitted' in v['body'] for v in e if v['kind'] in ('model.request','model.response','source.committed'))
 assert req('trace.config',capture=True)['ok'];assert not req('trace.config',quota=-1)['ok'];assert lib.dc_delete_chat(db,b'chat');assert not read('trace.export',chat='chat',segment=segment['id'])['ok'];assert lib.dc_add_chat(db,b'chat',b'New namespace',str(work).encode());assert read('trace.segments',chat='chat')['data']['items']==[]
 lib.dc_close(db)
 # Optional cache corruption cannot prevent a canonical chat opening/running.
 (state/'trajectory.sqlite').write_bytes(b'owned corrupt cache fixture')
 db=lib.dc_open(str(state).encode());assert db;assert not read('trace.status')['data']['available'];assert val(lib.dc_messages(db,b'chat'))==[];assert run('after corrupt cache')['status']=='completed';assert len(val(lib.dc_messages(db,b'chat')))==2;lib.dc_close(db)
 # Interrupted writer is produced by a genuinely killed owned process below.
 child='''import ctypes as C,sys,time
l=C.CDLL(sys.argv[1]);l.dl_trajectory_open.argtypes=[C.c_char_p];l.dl_trajectory_open.restype=C.c_void_p;l.dl_trajectory_begin.argtypes=[C.c_void_p,C.c_char_p,C.c_char_p,C.c_char_p,C.c_char_p,C.c_longlong,C.c_char_p];l.dl_trajectory_begin.restype=C.c_void_p
t=l.dl_trajectory_open(sys.argv[2].encode());assert t;assert l.dl_trajectory_begin(t,b'crash',b'run',b'model',b'chat',0,None);print('ready',flush=True);time.sleep(60)
'''
 child_dir=root/'crash';child_dir.mkdir(mode=0o700);child_path=root/'child.py';child_path.write_text(child)
 if os.environ.get('DUNLIN_SKIP_CRASH_CHILD')!='1':
  process=subprocess.Popen([sys.executable,'-B',str(child_path),str(pathlib.Path(sys.argv[1]).resolve()),str(child_dir)],stdout=subprocess.PIPE,text=True)
  assert process.stdout.readline().strip()=='ready';process.kill();process.wait();t=lib.dl_trajectory_open(str(child_dir).encode());assert t;lib.dl_trajectory_close(t)
  rows=sqlite3.connect(child_dir/'trajectory.sqlite').execute('SELECT status,duration,partial FROM segments').fetchall();assert rows==[('interrupted',None,1)],rows
  print('PASS killed owned writer retained as interrupted with unknown duration; no replay or invented completion')
 else:print('UNVERIFIED ASan child crash path excluded in this instrumented pass; separate normal fixture checks actual process crash')
 print('PASS disabled/capture policy, retained source, namespace rotation after deletion, negative settings and restart')
server.shutdown();server.server_close()

# Exercise the independent module's paging, structured redaction and bounded
# retention without network timing or touching any application history.
for name,args,result in [('json_tokener_parse',[S],P),('json_object_put',[P],C.c_int),('json_object_to_json_string_ext',[P,C.c_int],S)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=result
with tempfile.TemporaryDirectory(prefix='dunlin-trace-cache-') as td:
 root=pathlib.Path(td);root.chmod(0o700);cache=lib.dl_trajectory_open(str(root).encode());assert cache
 assert not lib.dl_trajectory_open(str(root).encode()),'a second writer must not mark an active owner interrupted'
 def direct(op,**kw):
  obj=lib.json_tokener_parse(json.dumps(dict(op=op,**kw)).encode());out=lib.dl_trajectory_request(cache,obj);assert out
  try:return json.loads(lib.json_object_to_json_string_ext(out,0))
  finally:lib.json_object_put(out);lib.json_object_put(obj)
 def begin(run):return lib.dl_trajectory_begin(cache,b'chat',run.encode(),b'fixture',b'chat',0,secret_bytes)
 def emit(span,kind,value):
  data=json.dumps(value,ensure_ascii=False).encode();lib.dl_trajectory_event(span,kind.encode(),data,len(data))
 span=begin('paging');assert span
 forbidden={'_dunlin_provider':{'encrypted_content':'OPAQUE-PRIVATE'},'signature':'PRIVATE-SIGNATURE','authorization':'STRUCTURED-TOKEN','api_key':'STRUCTURED-KEY','password':'USER-PASSWORD','safe':'中文 '+secret,'arguments':json.dumps({'access_token':'INNER-TOKEN','value':secret}),secret:'redacted-key'}
 emit(span,'model.request',forbidden)
 for i in range(124):emit(span,'test.event',{'number':i,'text':'中文原文'})
 emit(span,'reasoning.delta','PRIVATE_RAW_REASONING')
 lib.dl_trajectory_finish(span,b'completed',1)
 segment=direct('trace.segments',chat='chat')['data']['items'][0]['id'];ids=[];cursor=0
 while True:
  page=direct('trace.events',chat='chat',segment=segment,before=cursor)['data'];ids.extend(e['id'] for e in page['items'])
  if not page['more']:break
  cursor=page['nextCursor']
 assert len(ids)==125 and len(set(ids))==125 and ids==sorted(ids,reverse=True)
 output=direct('trace.export',chat='chat',segment=segment)['data'];raw=json.dumps(output)
 for token in ['OPAQUE-PRIVATE','PRIVATE-SIGNATURE','STRUCTURED-TOKEN','STRUCTURED-KEY','USER-PASSWORD','INNER-TOKEN','PRIVATE_RAW_REASONING',secret]:assert token not in raw,token
 assert '[REDACTED]' in raw
 for i in range(54):s=begin('extra-'+str(i));emit(s,'turn.start','input');lib.dl_trajectory_finish(s,b'completed',1)
 first=direct('trace.segments',chat='chat')['data'];second=direct('trace.segments',chat='chat',before=first['nextCursor'])['data'];assert first['more'] and len(first['items'])==50 and len(second['items'])==5 and not second['more']
 assert direct('trace.config',quota=1048576)['ok']
 for i in range(10):
  s=begin('quota-'+str(i));assert s
  for j in range(20):emit(s,'fixture.output',{'text':'中文'*12000})
  lib.dl_trajectory_finish(s,b'completed',1)
 status=direct('trace.status')['data'];assert status['payloadBytes']<=1048576,status
 latest=direct('trace.segments',chat='chat')['data']['items'][0];assert latest['partial'],'an active trace must be visibly partial when detail cannot fit'
 export=direct('trace.export',chat='chat',segment=latest['id'])['data'];assert export['partial']
 assert direct('trace.clear',chat='chat')['ok'] and direct('trace.segments',chat='chat')['data']['items']==[]
 lib.dl_trajectory_close(cache)
 print('PASS stable 50-event/segment paging, structured/nested/key redaction, private reasoning exclusion, UTF-8 byte quota, finished-only eviction, partial export and explicit cache-only clearing')
