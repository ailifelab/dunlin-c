#!/usr/bin/env python3
"""Actual owned C file effects, durable before/after, explicit native restore and faults."""
import ctypes as C,json,pathlib,tempfile,sys,hashlib,sqlite3,threading,http.server,os,subprocess,selectors
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_delete_chat',[P,S],C.c_int),('dc_workspace_request',[P,S],P),('dc_read_view',[S,S],P),('dc_run',[P,S,S,S,P],P)]:f=getattr(lib,name);f.argtypes=args;f.restype=result
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
allowed=True;reviews=[];requests=[]
@EV
def event(*_):pass
@AP
def approve(_,name,args):reviews.append((name.decode(),json.loads(args)));return int(allowed)
@CA
def cancel(_):return 0
hooks=Hooks(None,event,approve,cancel)
def val(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
def sha(text):return hashlib.sha256(text.encode()).hexdigest()
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  q=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(q);last=q['messages'][-1]
  if last['role']=='user' and last.get('content','').startswith('FILE '):
   action=json.loads(last['content'][5:]);msg={'role':'assistant','content':None,'tool_calls':[{'id':'file-call','type':'function','function':{'name':action.pop('tool','write_file'),'arguments':json.dumps(action,ensure_ascii=False)}}]};finish='tool_calls'
  else:msg={'role':'assistant','content':'Observed tool result.'};finish='stop'
  body=json.dumps({'choices':[{'index':0,'message':msg,'finish_reason':finish}]}).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='dunlin-file-history-') as tmp:
 root=pathlib.Path(tmp);state=root/'private';state.mkdir(mode=0o700);work=root/'project';work.mkdir();other=root/'other';other.mkdir();db=lib.dc_open(str(state).encode());assert db
 def req(op,**kw):return val(lib.dc_workspace_request(db,json.dumps(dict(op=op,**kw)).encode()))
 def read(op,**kw):return val(lib.dc_read_view(str(state).encode(),json.dumps(dict(op=op,**kw)).encode()))
 def project(id,path):
  r=req('projects.create',id=id,name=id,path=str(path),instructions='',model='',policy='ask',context=[],revision=0);assert r['ok'],r
 project('project',work);project('other',other)
 for chat in ('chat','fault','unknown','denied'):assert lib.dc_add_chat(db,chat.encode(),chat.encode(),str(work).encode())
 cfg=json.dumps({'base_url':f'http://127.0.0.1:{server.server_port}/v1','model':'owned-fixture','protocol':'chat','stream':False,'desktop_tools':'ask'}).encode()
 def run(chat='chat',**action):return val(lib.dc_run(db,chat.encode(),cfg,('FILE '+json.dumps(action,ensure_ascii=False)).encode(),C.byref(hooks)))
 def entries(chat='chat'):return req('receipts.list',chat=chat)['data']['items']
 def detail(id,chat='chat'):return req('receipts.detail',chat=chat,id=id)
 def preview(id,chat='chat',**kw):
  r=req('receipts.preview',chat=chat,id=id,**kw);assert r['ok'],r;return r['data']['plan']
 def restore(plan,chat='chat',**kw):return req('receipts.restore',chat=chat,plan=plan,confirmed=True,**kw)
 r=run(path='note.txt',text='v1 中文',expected_sha256='new');assert r['status']=='completed',r;first=entries()[0];assert first['state']=='applied' and first['grant']>0 and first['proposal']>=0 and first['call']=='file-call'
 assert (work/'note.txt').read_text()=='v1 中文';source=detail(first['id'])['data'];assert source['before']=='' and source['after']=='v1 中文' and not source['existed'] and source['canRestore']
 assert read('receipts.list',chat='chat')['data']['items'][0]['id']==first['id'];assert read('receipts.detail',chat='chat',id=first['id'])['data']['after']=='v1 中文';assert read('grants.list',chat='chat')['data'][0]['id']==first['grant'];assert not read('receipts.preview',chat='chat',id=first['id'])['ok'] and not read('receipts.restore',chat='chat',plan='nope',confirmed=True)['ok']
 assert not detail(first['id'],chat='fault')['ok'];assert req('receipts.preview',chat='chat',id=first['id'],acknowledgeUnknown='false')['error']=='invalid_file_history_acknowledgement'
 r=run(path='note.txt',text='v2 中文',expected_sha256=sha('v1 中文'));assert r['status']=='completed',r;second=entries()[0];source=detail(second['id'])['data'];assert source['before']=='v1 中文' and source['after']=='v2 中文' and source['beforeHash']==sha('v1 中文') and source['afterHash']==sha('v2 中文')
 plan=preview(second['id']);(work/'note.txt').write_text('external edit');r=restore(plan);assert not r['ok'] and (work/'note.txt').read_text()=='external edit',r;assert not detail(second['id'])['data']['canRestore']
 (work/'note.txt').write_text('v2 中文');reused=restore(plan);assert not reused['ok'],'a confirmed conflict attempt must consume its plan: '+repr(reused)
 expired=preview(second['id']);sql=sqlite3.connect(state/'conversations.sqlite');sql.execute('UPDATE file_restore_plans SET expires=unixepoch()-1 WHERE id=?',(expired,));sql.commit();assert not restore(expired)['ok']
 plan=preview(second['id']);assert not req('receipts.restore',chat='chat',plan=plan,confirmed='false')['ok'];restored=restore(plan);assert restored['ok'];assert (work/'note.txt').read_text()=='v1 中文';assert not restore(plan)['ok'];undo=entries()[0];assert undo['parent']==second['id'] and undo['operation']=='restore_file' and restored['data']['operationId']==undo['id'];assert restore(preview(undo['id']))['ok'];assert (work/'note.txt').read_text()=='v2 中文'
 print('PASS C approved file effects, complete immutable before/after and exact grant/run/proposal/call references; explicit one-use restore, expiry and external-edit conflict')
 assert run(path='new.txt',text='new file',expected_sha256='new')['status']=='completed';new=entries()[0];assert restore(preview(new['id']))['ok'];assert not (work/'new.txt').exists();removed=entries()[0];assert not removed['afterExists']
 native=req('trace.segments',chat='chat')['data']['items'][0];assert native['protocol']=='host' and native['status']=='completed';native_events=req('trace.events',chat='chat',segment=native['id'])['data']['items'];intent=next(e['body']for e in native_events if e['kind']=='file.operation.request');assert not intent['targetExists'] and intent['targetHash']=='absent'
 assert restore(preview(removed['id']))['ok'] and (work/'new.txt').read_text()=='new file'
 big='HEAD'+'a'*100000+'TAIL';(work/'large.txt').write_text(big);r=run(tool='patch_file',path='large.txt',old_text='HEAD',new_text='BEGIN',expected_sha256=sha(big));assert r['status']=='completed',r;large=entries()[0];view=detail(large['id'])['data'];assert view['before']==big and view['canRestore'];assert restore(preview(large['id']))['ok'] and (work/'large.txt').read_text()==big
 r=req('chats.update',id='chat',field='project',value='other');assert r['ok'],r;assert not detail(second['id'])['data']['canRestore'];assert req('chats.update',id='chat',field='project',value='project')['ok']
 os.link(work/'note.txt',work/'note-hardlink.txt');assert not detail(second['id'])['data']['canRestore'];(work/'note-hardlink.txt').unlink();(work/'link').symlink_to(other,target_is_directory=True);r=run(path='link/escape.txt',text='must not escape',expected_sha256='new');assert r['status']=='failed' and not (other/'escape.txt').exists(),r
 print('PASS new-file removal/redo, large controlled text patch/restore, project reassociation guards, hardlink/symlink rejection')
 allowed=False;r=run(chat='denied',path='denied.txt',text='no',expected_sha256='new');allowed=True;assert r['status']=='denied' and not entries('denied') and not (work/'denied.txt').exists()
 sql.execute("CREATE TRIGGER owned_history_prepare_fault BEFORE INSERT ON file_operations WHEN NEW.chat='fault' BEGIN SELECT RAISE(ABORT,'owned fixture history preparation failure');END");sql.commit();r=run(chat='fault',path='no-effect.txt',text='must not write',expected_sha256='new');assert r['status']=='failed' and not (work/'no-effect.txt').exists() and not entries('fault'),r;assert not list(work.glob('.dunlin-*'));sql.execute('DROP TRIGGER owned_history_prepare_fault');sql.commit()
 sql.execute("CREATE TRIGGER owned_history_settle_fault BEFORE UPDATE OF state ON file_operations WHEN NEW.chat='unknown' AND NEW.state='applied' BEGIN SELECT RAISE(ABORT,'owned fixture history settlement failure');END");sql.commit();r=run(chat='unknown',path='unknown.txt',text='actual effect',expected_sha256='new');assert r['status']=='unknown' and (work/'unknown.txt').read_text()=='actual effect',r;unknown=entries('unknown')[0];assert unknown['state']=='prepared';assert not lib.dc_delete_chat(db,b'unknown');sql.execute('DROP TRIGGER owned_history_settle_fault');sql.commit();sql.close();lib.dc_close(db)
 db=lib.dc_open(str(state).encode());assert db;unknown=entries('unknown')[0];assert unknown['state']=='unknown';recovery=req('recovery.inspect',chat='unknown')['data'];assert req('recovery.abandon',chat='unknown',stateHash=recovery['stateHash'],acknowledgeUnknown=True)['ok'];assert not req('receipts.preview',chat='unknown',id=unknown['id'],acknowledgeUnknown='false')['ok'];plan=preview(unknown['id'],chat='unknown',acknowledgeUnknown=True);assert restore(plan,chat='unknown')['ok'];assert not (work/'unknown.txt').exists()
 print('PASS denial/preparation failure has no file effect; actual write with failed audit settlement remains unknown across restart; explicit acknowledgement and native restore do not replay model command')
 sql=sqlite3.connect(state/'conversations.sqlite');blob=sql.execute('SELECT after_hash FROM file_operations WHERE id=?',(second['id'],)).fetchone()[0];sql.execute("UPDATE file_history_blobs SET text='tampered' WHERE hash=?",(blob,));sql.commit();assert not detail(second['id'])['ok'];before=(work/'note.txt').read_text();r=run(path='note.txt',text='v3',expected_sha256=sha(before));assert r['status']=='failed' and (work/'note.txt').read_text()==before,r
 for chat in ('chat','fault','unknown','denied'):assert lib.dc_delete_chat(db,chat.encode())
 assert sql.execute('SELECT count(*) FROM file_operations').fetchone()[0]==0 and sql.execute('SELECT count(*) FROM file_history_blobs').fetchone()[0]==0 and sql.execute('SELECT count(*) FROM file_restore_plans').fetchone()[0]==0;assert (work/'note.txt').read_text()==before;sql.close();lib.dc_close(db)
 print('PASS snapshot corruption refuses restore/new publication; chat deletion cleans owned histories/plans/unreferenced blobs and preserves project files')
 # Ordinary chats also capture a workspace identity; they have no project row.
 ordinary=root/'ordinary';ordinary.mkdir();db=lib.dc_open(str(state).encode());assert db
 assert lib.dc_add_chat(db,b'ordinary',b'Ordinary workspace',str(ordinary).encode())
 receipt_input=dict(path='scratch.txt',text='owned ordinary file',expected_sha256='new')
 assert run(chat='ordinary',**receipt_input)['status']=='completed'
 receipt=entries('ordinary')[0]
 assert detail(receipt['id'],'ordinary')['data']['canRestore']
 moved=root/'moved-ordinary';ordinary.rename(moved);ordinary.mkdir()
 assert detail(receipt['id'],'ordinary')['data']['currentState']=='project_unavailable'
 ordinary.rmdir();moved.rename(ordinary)
 assert restore(preview(receipt['id'],'ordinary'),'ordinary')['ok'] and not (ordinary/'scratch.txt').exists()
 lib.dc_close(db)
 print('PASS ordinary chat receipt read and rollback use captured workspace identity; substituted directory is refused.')
server.shutdown();server.server_close()

if os.environ.get('DUNLIN_SKIP_CRASH_CHILD')=='1':
 print('UNVERIFIED sanitizer child crash path omitted here; normal fixture checks genuine killed C process and partial-stage cleanup')
else:
 with tempfile.TemporaryDirectory(prefix='dunlin-file-crash-') as tmp:
  root=pathlib.Path(tmp);library=pathlib.Path(sys.argv[1]).resolve();binary=root/'owned-file-crash'
  subprocess.run(['cc','-std=c11','-D_DARWIN_C_SOURCE','-Iinclude','-I/opt/homebrew/opt/json-c/include','tests/file_history_crash.c',str(library),'-Wl,-headerpad_max_install_names','-o',str(binary)],check=True)
  # The caller may provide a content-addressed immutable dylib rather than a
  # file named libdunlin-test.dylib. Pin the child to that exact supplied image;
  # do not silently load the builder's mutable original install-name path.
  if sys.platform=='darwin':
   install_name=subprocess.check_output(['otool','-D',str(library)],text=True).splitlines()[1].strip()
   subprocess.run(['install_name_tool','-change',install_name,str(library),str(binary)],check=True)
   subprocess.run(['codesign','--force','--sign','-',str(binary)],check=True)
   assert str(library) in subprocess.check_output(['otool','-L',str(binary)],text=True)
  for scenario in ('partial','unmatched'):
   private=root/(scenario+'-state');private.mkdir(mode=0o700);work=root/(scenario+'-project');work.mkdir();db=lib.dc_open(str(private).encode());assert db
   body={'op':'projects.create','id':'crash-project','name':'Crash fixture','path':str(work),'instructions':'','model':'','policy':'ask','context':[],'revision':0};assert val(lib.dc_workspace_request(db,json.dumps(body).encode()))['ok'];assert lib.dc_add_chat(db,b'crash',b'Crash fixture',str(work).encode());lib.dc_close(db)
   process=subprocess.Popen([str(binary),str(private),str(work)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True);selector=selectors.DefaultSelector();selector.register(process.stdout,selectors.EVENT_READ)
   ready=selector.select(10)
   if not ready:process.kill();_,stderr=process.communicate();raise AssertionError('owned C child failed to prepare: '+stderr)
   line=process.stdout.readline().strip();assert line.startswith('PREPARED '),line;operation=line.split()[1];process.kill();process.communicate(timeout=5);selector.close()
   assert not (work/'crash.txt').exists() and not list(work.glob('.dunlin-*')),'preparation must precede any staging/publication'
   sql=sqlite3.connect(private/'conversations.sqlite');row=sql.execute('SELECT state,after_hash FROM file_operations WHERE id=?',(operation,)).fetchone();assert row[0]=='prepared';assert sql.execute('SELECT count(*) FROM file_stage_cleanup').fetchone()[0]==1;sql.close()
   # Materialize a partial staging artifact as a distinct controlled recovery
   # input, including an incomplete UTF-8 code point. This is not a claim that
   # the process was killed during its actual write syscall.
   stage=work/('.dunlin-'+operation);intended='partial 中文 body'.encode();stage.write_bytes(intended[:10] if scenario=='partial' else b'unrelated-user-content');stage.chmod(0o600);unrelated=work/'.dunlin-not-owned-by-this-run';unrelated.write_text('keep this unowned file')
   db=lib.dc_open(str(private).encode());assert db
   def local(op,**kw):return val(lib.dc_workspace_request(db,json.dumps(dict(op=op,**kw)).encode()))
   assert local('receipts.list',chat='crash')['data']['items'][0]['state']=='unknown';assert not (work/'crash.txt').exists() and unrelated.read_text()=='keep this unowned file'
   if scenario=='partial':assert not stage.exists() and local('receipts.status')['data']['pendingStages']==0
   else:
    assert stage.read_bytes()==b'unrelated-user-content' and local('receipts.status')['data']['pendingStages']==1;assert local('receipts.cleanup')['data']['pendingStages']==1
    stage.write_bytes(intended[:10]);assert local('receipts.cleanup')['data']['pendingStages']==0 and not stage.exists()
   assert lib.dc_delete_chat(db,b'crash');assert not (work/'crash.txt').exists() and unrelated.exists();lib.dc_close(db)
  print('PASS genuinely killed C process after durable authorization/preparation; no original publication/replay, partial UTF-8 staging recovery, unmatched/unowned files retained and later verified cleanup')
