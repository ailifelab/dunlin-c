#!/usr/bin/env python3
"""Per-turn project-reference selection; owned files/model fixture, no tools."""
import ctypes as C,json,pathlib,tempfile,sys,http.server,threading,hashlib,sqlite3,subprocess,os
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_workspace_request',[P,S],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_run_with_secret',[P,S,S,S,P,S],P),('dc_messages',[P,S],P),('dc_chats',[P],P)]:
 fn=getattr(lib,name);fn.argtypes=args;fn.restype=result
def encode(v):return json.dumps(v,ensure_ascii=False).encode()
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
if '--child' in sys.argv:
 db=lib.dc_open(sys.argv[3].encode());assert db
 lib.dc_run_with_secret(db,b'frozen',sys.argv[4].encode(),b'Frozen original',None,b'')
 raise SystemExit('owned child returned before kill')
seen=[];errors=[];hold=False;entered=threading.Event();release=threading.Event()
class Server(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  try:
   request=json.loads(self.rfile.read(int(self.headers['Content-Length'])));seen.append(request)
   if hold:entered.set();release.wait(10)
   body=encode(dict(choices=[dict(index=0,message=dict(role='assistant',content='fixture-reply'),finish_reason='stop')]))
   self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers()
   try:self.wfile.write(body)
   except (BrokenPipeError,ConnectionResetError):pass
  except BaseException as e:errors.append(e)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Server);threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='dunlin-context-selection-') as tmp:
 root=pathlib.Path(tmp);private=root/'private';private.mkdir(mode=0o700);work=root/'work';work.mkdir();db=lib.dc_open(str(private).encode());assert db
 (work/'first.txt').write_text('FIRST_SELECTED_SOURCE\n');(work/'second.txt').write_text('SECOND_UNSELECTED_SOURCE\n');(work/'not-pinned.txt').write_text('MUST_NOT_READ')
 def req(op,**kw):return value(lib.dc_workspace_request(db,encode(dict(op=op,**kw))))
 def ok(op,**kw):
  result=req(op,**kw);assert result['ok'],result;return result.get('data')
 project=ok('projects.create',id='project',name='Fixture Project',path=str(work),instructions='',model='',policy='noTools',context=['first.txt','second.txt'],bookmark='')
 for chat in ['empty','selected','changed','invalid','frozen','legacy']:assert lib.dc_add_chat(db,chat.encode(),chat.encode(),str(work).encode())
 cfg=dict(base_url=f'http://127.0.0.1:{server.server_port}/v1',model='fixture',protocol='chat',stream=False,desktop_tools='noTools',desktop_auth='none',desktop_profile_id='fixture',desktop_credential_revision=0)
 def run(chat,selection,text='Original exact 用户文本。',legacy=False):
  settings=dict(cfg)
  if not legacy:settings['desktop_project_context']=selection
  return value(lib.dc_run_with_secret(db,chat.encode(),encode(settings),text.encode() if text is not None else None,None,b''))
 def ref(path):return dict(path=path,sha256=hashlib.sha256((work/path).read_bytes()).hexdigest())
 # Even an unreadable/deleted unselected pin must not block an empty selection.
 (work/'second.txt').unlink();result=run('empty',[]);assert result['status']=='completed',result
 assert seen[-1]['messages'][-1]['content']=='Original exact 用户文本。' and 'FIRST_SELECTED_SOURCE' not in json.dumps(seen[-1])
 selected=ref('first.txt');result=run('selected',[selected]);assert result['status']=='completed',result
 rows=ok('files.references',project='project');assert [r['state'] for r in rows]==['unverified','unavailable'] and 'text' not in json.dumps(rows),rows
 project=ok('projects.update',**project,contextVersions={'first.txt':selected['sha256']});rows=ok('files.references',project='project');assert rows[0]['state']=='current' and rows[0]['baselineSHA']==selected['sha256'],rows
 content=seen[-1]['messages'][-1]['content'];assert 'FIRST_SELECTED_SOURCE' in content and selected['sha256'] in content and 'reference-data' in content and 'SECOND_UNSELECTED_SOURCE' not in content
 assert value(lib.dc_messages(db,b'selected'))[0]['content']=='Original exact 用户文本。'
 # Subsequent empty choice must not append files again or mutate prior messages.
 first=seen[-1]['messages'][-1]['content'];assert run('selected',[],text='Next message')['status']=='completed';assert seen[-1]['messages'][-1]['content']=='Next message' and any(m.get('content')==first for m in seen[-1]['messages'])
 before=len(seen);(work/'first.txt').write_text('CHANGED_SOURCE\n');result=run('changed',[selected]);assert result['status']=='failed' and len(seen)==before and '变化' in result['text'],result
 assert ok('files.references',project='project')[0]['state']=='changed'
 for versions in [None,{'first.txt':selected['sha256']},{'not-pinned.txt':ref('not-pinned.txt')['sha256']},{'first.txt':123}]:
  result=req('projects.update',**project,contextVersions=versions);assert not result['ok'] and result['error']=='reference_version_conflict',result
 assert ok('projects.list')[0]['revision']==project['revision']
 with sqlite3.connect(private/'conversations.sqlite') as sql:
  sql.execute("CREATE TRIGGER fail_baseline BEFORE INSERT ON project_reference_baselines BEGIN SELECT RAISE(ABORT,'owned fixture'); END")
 result=req('projects.update',**dict(project,name='must roll back'),contextVersions={'first.txt':ref('first.txt')['sha256']});assert not result['ok'];assert ok('projects.list')[0]['name']=='Fixture Project'
 with sqlite3.connect(private/'conversations.sqlite') as sql:sql.execute('DROP TRIGGER fail_baseline')
 for selection in [None,[ref('not-pinned.txt')],[ref('first.txt'),ref('first.txt')],[dict(path='../outside',sha256='0'*64)],[dict(path='first.txt',sha256='new')],dict(path='first.txt'),[dict(ref('first.txt'),extra=True)],[],[ref('first.txt')]*25]:
  if selection==[]:continue
  before=len(seen);result=run('invalid',selection);assert result['status']=='failed' and len(seen)==before,(selection,result)
 print('PASS exact per-turn pinned selection; empty means no source reads, selected hashes appear as reference data, original user display/history preserved, stale/unpinned/duplicate/unsafe/malformed choices block before HTTP')
 # A process interruption leaves an admitted active turn, unlike a terminal
 # HTTP failure. Resume must reuse exactly the original expanded user content.
 if not os.environ.get('DUNLIN_SKIP_CRASH_CHILD'):
  frozen=ref('first.txt');lib.dc_close(db);hold=True;entered.clear();release.clear()
  child=subprocess.Popen([sys.executable,'-B',__file__,sys.argv[1],'--child',str(private),json.dumps(dict(cfg,desktop_project_context=[frozen]))],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  try:
   assert entered.wait(4),'owned child did not reach model'
   assert 'CHANGED_SOURCE' in seen[-1]['messages'][-1]['content'];child.kill();child.communicate(timeout=3);assert child.returncode<0
  finally:
   hold=False;release.set()
   if child.poll() is None:child.kill();child.communicate(timeout=3)
  (work/'first.txt').write_text('LATER_EXTERNAL_CHANGE\n');(work/'second.txt').write_text('RESTORED_PIN\n');db=lib.dc_open(str(private).encode());assert db
  result=run('frozen',[],text=None);assert result['status']=='completed',result
  content=seen[-1]['messages'][-1]['content'];assert 'CHANGED_SOURCE' in content and 'LATER_EXTERNAL_CHANGE' not in content and 'RESTORED_PIN' not in content
  assert value(lib.dc_messages(db,b'frozen'))[0]['content']=='Frozen original'
 else:(work/'second.txt').write_text('RESTORED_PIN\n')
 assert run('legacy',None,legacy=True)['status']=='completed';assert 'RESTORED_PIN' in seen[-1]['messages'][-1]['content']
 chats=value(lib.dc_chats(db));assert next(c for c in chats if c['id']=='selected')['lastStatus']=='completed' and next(c for c in chats if c['id']=='selected')['updated']>0
 lib.dc_close(db);db=lib.dc_open(str(private).encode());assert db;assert value(lib.dc_messages(db,b'selected'))[0]['content']=='Original exact 用户文本。';assert ok('files.references',project='project')[0]['state']=='changed'
 project=ok('projects.list')[0];project=ok('projects.update',**project,contextVersions={'first.txt':ref('first.txt')['sha256']});assert ok('files.references',project='project')[0]['state']=='current'
 project=ok('projects.update',**project,relink=True);assert ok('files.references',project='project')[0]['state']=='unverified'
 project=ok('projects.update',**dict(project,context=['second.txt']));assert [r['path'] for r in ok('files.references',project='project')]==['second.txt']
 ok('projects.remove',id='project',revision=project['revision'],mode='keep')
 with sqlite3.connect(private/'conversations.sqlite') as sql:assert sql.execute('SELECT count(*) FROM project_reference_baselines').fetchone()[0]==0
 assert (work/'first.txt').exists() and (work/'second.txt').exists();lib.dc_close(db);assert not errors,errors
 print('PASS legacy no-field C behavior and original display survive restart; owned crash/frozen resume '+('skipped under sanitizer' if os.environ.get('DUNLIN_SKIP_CRASH_CHILD') else 'verified without rereading changed files or adopting new selection'))
 print('PASS baseline hash acknowledgement CAS, missing/changed/legacy states, rollback on storage failure, persistent metadata and relink/remove pruning without deleting source files; chat status/time originate in C records')
server.shutdown();server.server_close()
