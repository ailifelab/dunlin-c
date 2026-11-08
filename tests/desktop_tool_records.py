#!/usr/bin/env python3
"""C source-owned tool records with actual isolated file/process effects."""
import ctypes as C
import hashlib
import http.server
import json
import pathlib
import sqlite3
import sys
import tempfile
import threading
import time
import os
import subprocess

lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_delete_chat',[P,S],C.c_int),('dc_workspace_request',[P,S],P),('dc_read_view',[S,S],P),('dc_run',[P,S,S,S,P],P)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=result
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
allow=True;block=False;waiting=threading.Event();release=threading.Event();requests=[]
@EV
def event(_,name,text):
 if len(sys.argv)>2 and sys.argv[2]=='--child-after-effect' and name==b'tool.post_execute':
  pathlib.Path(sys.argv[5]).write_text('real tool returned before canonical result checkpoint')
  time.sleep(20)
@AP
def approval(_,name,args):
 if block:
  waiting.set();assert release.wait(8)
 return int(allow)
@CA
def cancelled(_):return 0
hooks=Hooks(None,event,approval,cancelled)
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
def call(name,args,id='reused-call'):
 return dict(id=id,type='function',function=dict(name=name,arguments='  '+json.dumps(args,ensure_ascii=False)+'\n'))
if len(sys.argv)>2 and sys.argv[2]=='--child-after-effect':
 db=lib.dc_open(sys.argv[3].encode());assert db
 print(value(lib.dc_run(db,b'crash',sys.argv[4].encode(),b'WRITE crash.txt',C.byref(hooks))),flush=True)
 raise SystemExit(5)
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  request=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(request)
  messages=request['messages'];last_user=max(i for i,m in enumerate(messages) if m['role']=='user');prompt=messages[last_user]['content'];tail=messages[last_user+1:];calls=[]
  if not tail:
   if prompt=='repeat':calls=[call('write_file',dict(path='repeat.txt',text='source-owned output',expected_sha256='new'))]
   elif prompt=='failed-command':calls=[call('shell',dict(command="printf TOOL_RECORDS_OK; printf fixture-error >&2; exit 3"))]
   elif prompt=='missing':calls=[call('read_file',dict(path='missing.txt'))]
   elif prompt=='read':calls=[call('read_file',dict(path='repeat.txt'))]
   elif prompt.startswith('WRITE '):calls=[call('write_file',dict(path=prompt[6:],text='do not invent completion',expected_sha256='new'))]
  elif prompt=='repeat' and len([m for m in tail if m['role']=='tool'])==1:
   calls=[call('read_file',dict(path='repeat.txt'))]
  message=dict(role='assistant',content=None,tool_calls=calls) if calls else dict(role='assistant',content='Observed actual fixture tool result.')
  body=json.dumps(dict(choices=[dict(index=0,message=message,finish_reason='tool_calls' if calls else 'stop')])).encode();self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='dunlin-tool-records-') as tmp:
 root=pathlib.Path(tmp);state=root/'state';state.mkdir(mode=0o700);work=root/'project';work.mkdir();db=lib.dc_open(str(state).encode());assert db
 def req(op,**kw):return value(lib.dc_workspace_request(db,json.dumps(dict(op=op,**kw)).encode()))
 def read(op,**kw):return value(lib.dc_read_view(str(state).encode(),json.dumps(dict(op=op,**kw)).encode()))
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r.get('data')
 assert req('projects.create',id='project',name='Fixture project',path=str(work),instructions='',model='',policy='ask',context=[])['ok']
 for chat in ['normal','denied','live','unknown','audit-fault','other','crash']:
  assert lib.dc_add_chat(db,chat.encode(),chat.encode(),str(work).encode())
 config=dict(base_url=f'http://127.0.0.1:{server.server_port}/v1',model='fixture-A',protocol='chat',stream=False,desktop_tools='ask')
 def run(prompt,chat='normal',model='fixture-A'):
  return value(lib.dc_run(db,chat.encode(),json.dumps(dict(config,model=model)).encode(),prompt.encode(),C.byref(hooks)))
 def rows(chat='normal',**kw):return read('tools.list',chat=chat,**kw)['data']['items']
 def detail(row):
  r=read('tools.detail',chat=row['chat'],id=row['id']);assert r['ok'],r;return r['data']
 ok('trace.config',enabled=False,capture=True,quota=1048576)
 result=run('repeat');assert result['status']=='completed',result;assert (work/'repeat.txt').read_text()=='source-owned output'
 records=rows();assert len(records)==2 and len({r['run'] for r in records})==1 and len({r['sourceSequence'] for r in records})==2 and all(r['callID']=='reused-call' for r in records)
 for row in records:
  d=detail(row);assert d['stage']=='committed' and d['outcome']=='completed' and d['resultSequence']>d['sourceSequence'] and d['executionMs']>=0 and d['committedMs']>=d['executionMs']
  assert d['arguments'].startswith('  ') and d['arguments'].endswith('\n') and json.loads(d['schema'])['function']['name']==d['name']
 write=next(detail(r) for r in records if r['name']=='write_file');assert write['grantID']>0 and write['waitMs']>=0 and write['approvedArguments']==write['arguments'] and write['fileReceiptID'] and write['grantStatus']=='expired'
 assert read('tools.locate',chat='normal',sequence=write['resultSequence'])['data']['items'][0]['id']==write['id']
 assert not read('tools.detail',chat='other',id=write['id'])['ok']
 assert read('trace.segments',chat='normal')['data']['items']==[]
 visible=read('chat.messages',chat='normal')['data'];assert len(visible)==4 and len([m for m in visible if m['role']=='tool'])==2
 with sqlite3.connect(state/'conversations.sqlite') as sql:assert sql.execute("SELECT json_array_length(json_extract(state,'$.messages')) FROM chats WHERE id='normal'").fetchone()[0]==6
 print('PASS real file/read effects; complete original arguments, scoped repeated call IDs, exact statuses/schema/source/grant/receipt links independent of optional trace')
 # Runtime requests one correction after a known nonzero exit. This fixture
 # returns no correction call, so the overall turn fails while the tool's
 # precise execution_failed outcome and output remain independently recorded.
 failed=run('failed-command');assert failed['status']=='failed',failed
 command=detail(rows()[0]);assert command['stage']=='committed' and command['outcome']=='execution_failed' and 'TOOL_RECORDS_OK' in command['result'] and 'fixture-error' in command['result']
 missing=run('missing');assert missing['status']!='completed';assert detail(rows()[0])['outcome']!='completed'
 allow=False;denied=run('WRITE denied.txt','denied');allow=True;assert denied['status']=='denied' and not (work/'denied.txt').exists();d=detail(rows('denied')[0]);assert d['stage']=='denied' and d['outcome']=='denied' and 'executionMs' not in d
 print('PASS committed failed command output remains failure; refusal has no file effect or invented execution time')
 block=True;waiting.clear();release.clear();answers=[];thread=threading.Thread(target=lambda:answers.append(run('WRITE live.txt','live')));thread.start()
 try:
  assert waiting.wait(8)
  # Keep the original positive-wait assertion meaningful: the read-only queries
  # can finish within one millisecond on this host. This is a controlled fixture
  # interval, not a production timing floor or a performance budget.
  hold_until=time.monotonic()+0.010
  pending=detail(rows('live')[0]);assert pending['stage']=='awaiting' and 'result' not in pending and 'executionMs' not in pending and 'waitMs' not in pending
  assert read('tools.list',chat='other')['data']['items']==[]
  while True:
   remaining=hold_until-time.monotonic()
   if remaining<=0:break
   time.sleep(remaining)
 finally:release.set();thread.join(10);block=False
 assert not thread.is_alive(),('live writer did not return',answers)
 assert answers and answers[0]['status']=='completed',('live writer outcome',answers)
 settled=detail(rows('live')[0])
 assert settled['stage']=='committed',('live settlement stage',settled)
 assert settled['waitMs']>0,('expected positive approval interval',settled)
 assert settled['resultSequence']>settled['sourceSequence'],('invalid result pairing',settled)
 print('PASS independent read-only WAL inspection during blocked approval, no fabricated running duration or cross-chat data')
 original_run=write['run'];assert len(rows(run=original_run))==2 and rows(query='TOOL_RECORDS_OK')[0]['name']=='shell' and rows(stage='denied')==[]
 for _ in range(52):assert run('read')['status']=='completed'
 ok('trace.config',enabled=True)
 assert run('read',model='fixture-B')['status']=='completed';latest=detail(rows()[0]);assert latest['run']!=original_run
 segments=read('trace.segments',chat='normal',run=latest['run'])['data']['items'];assert segments and all(s['run']==latest['run'] and s['model']=='fixture-B' for s in segments)
 switched=next(r for r in reversed(requests) if r['model']=='fixture-B')
 assert any(m['role']=='tool' and m.get('content')==write['result'] for m in switched['messages'])
 assert any(m['role']=='user' and m.get('content')=='repeat' for m in switched['messages'])
 first=read('tools.list',chat='normal')['data'];assert len(first['items'])==50 and first['more']
 older=read('tools.list',chat='normal',before=first['nextCursor'])['data'];assert len(older['items'])==7 and not older['more'] and not set(r['id'] for r in first['items'])&set(r['id'] for r in older['items'])
 for kw in [dict(query=3),dict(query='x\x00y'),dict(before=True),dict(before=-1),dict(stage=[]),dict(run='x'*65)]:assert not read('tools.list',chat='normal',**kw)['ok']
 print('PASS stable 50-call paging, filters/search/type rejection, indexed run deep link and switched model retains earlier tool facts')
 with sqlite3.connect(state/'conversations.sqlite') as sql:
  sql.execute("CREATE TRIGGER reject_tool_audit BEFORE UPDATE OF stage ON tool_records WHEN NEW.chat='audit-fault' AND NEW.stage='validating' BEGIN SELECT RAISE(ABORT,'fixture audit admission');END")
 r=run('WRITE no-effect.txt','audit-fault');assert r['status']!='completed' and not (work/'no-effect.txt').exists()
 with sqlite3.connect(state/'conversations.sqlite') as sql:
  sql.execute('DROP TRIGGER reject_tool_audit')
  sql.execute("CREATE TRIGGER reject_tool_source BEFORE UPDATE OF state ON chats WHEN NEW.id='unknown' AND json_array_length(json_extract(NEW.state,'$.messages'))>2 BEGIN SELECT RAISE(ABORT,'fixture result commit');END")
 r=run('WRITE unknown.txt','unknown');assert r['status']=='unknown' and (work/'unknown.txt').exists();u=detail(rows('unknown')[0]);assert u['unknownEffect'] and u['stage']=='unknown' and 'resultSequence' not in u
 lib.dc_close(db);db=lib.dc_open(str(state).encode());assert db;assert detail(rows('unknown')[0])['unknownEffect'];assert len(rows())==50
 with sqlite3.connect(state/'conversations.sqlite') as sql:sql.execute('DROP TRIGGER reject_tool_source')
 before=len(requests);recovery=ok('recovery.inspect',chat='unknown');ok('recovery.abandon',chat='unknown',stateHash=recovery['stateHash'],acknowledgeUnknown=True);assert len(requests)==before and (work/'unknown.txt').exists()
 abandoned=detail(rows('unknown')[0]);assert abandoned['stage']=='unknown' and abandoned['unknownEffect'] and abandoned['resultSequence']>abandoned['sourceSequence'] and 'UNKNOWN external effect' in abandoned['result']
 if not os.environ.get('DUNLIN_SKIP_CRASH_CHILD'):
  lib.dc_close(db);marker=root/'after-effect.marker'
  child=subprocess.Popen([sys.executable,__file__,str(pathlib.Path(sys.argv[1]).resolve()),'--child-after-effect',str(state),json.dumps(config),str(marker)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  try:
   deadline=time.monotonic()+8
   while not marker.exists() and child.poll() is None and time.monotonic()<deadline:time.sleep(.01)
   assert marker.exists(),'owned C writer did not reach actual post-effect boundary'
  finally:
   if child.poll() is None:child.kill()
   child.communicate(timeout=5)
  assert (work/'crash.txt').read_text()=='do not invent completion'
  db=lib.dc_open(str(state).encode());assert db;crashed=detail(rows('crash')[0]);assert crashed['stage']=='interrupted' and crashed['unknownEffect'] and 'resultSequence' not in crashed and 'executionMs' not in crashed
  before=len(requests);reply=value(lib.dc_run(db,b'crash',json.dumps(config).encode(),None,C.byref(hooks)));assert reply['status']=='unknown' and len(requests)==before and len(rows('crash'))==1
  print('PASS genuinely killed owned writer after actual file effect and before source result; restart exposes unknown without invented outcome/time, duplicate call or replay')
 else:print('UNVERIFIED sanitizer child crash path omitted; normal fixture genuinely kills writer after actual file effect')
 assert lib.dc_delete_chat(db,b'normal');assert not read('tools.list',chat='normal')['ok'];assert (work/'repeat.txt').exists()
 with sqlite3.connect(state/'conversations.sqlite') as sql:
  assert sql.execute("SELECT count(*) FROM tool_records WHERE chat='normal'").fetchone()[0]==0
  assert sql.execute('SELECT count(*) FROM tool_record_schemas WHERE record NOT IN(SELECT id FROM tool_records)').fetchone()[0]==0
 lib.dc_close(db)
 print('PASS audit admission failure prevents effect; actual effect with failed source settlement remains unknown across reopen; explicit abandonment never replays; deletion cleans owned records but preserves project files')
server.shutdown()
