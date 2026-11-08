#!/usr/bin/env python3
"""Captured chat workspace versus project default; owned folders, no real model."""
import desktop_fixture_sqlite as fixture_sqlite
import ctypes as C,hashlib,http.server,json,pathlib,sqlite3,sys,tempfile,threading,subprocess,shutil,os
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for name,args,ret in [('dc_open',[S],P),('dc_close',[P],None),('dc_workspace_request',[P,S],P),('dc_read_view',[S,S],P),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_rename_chat',[P,S,S],C.c_int),('dc_chats',[P],P),('dc_messages',[P,S],P),('dc_run_with_secret',[P,S,S,S,P,S],P)]:
    f=getattr(lib,name);f.argtypes=args;f.restype=ret
def enc(x):return json.dumps(x,ensure_ascii=False).encode()
def value(p):
    assert p
    try:return json.loads(C.string_at(p))
    finally:lib.dc_free(p)
if '--old-child' in sys.argv:
    db=lib.dc_open(sys.argv[3].encode())
    try:
        result=value(lib.dc_run_with_secret(db,b'model-old',sys.argv[4].encode(),b'UNSAFE_OLD_VERSION_ATTEMPT',None,b'')) if db else None
        print(json.dumps(dict(opened=bool(db),result=result)),flush=True)
    finally:
        if db:lib.dc_close(db)
    raise SystemExit(0)
if '--legacy-create' in sys.argv:
    state,work,cfg=sys.argv[3:6];db=lib.dc_open(state.encode());assert db
    try:
        r=value(lib.dc_workspace_request(db,enc(dict(op='projects.create',id='legacy-project',name='Legacy project',path=work,instructions='',model='',policy='ask',context=[]))));assert r['ok'],r
        assert lib.dc_add_chat(db,b'legacy',b'Legacy source',work.encode())
        result=value(lib.dc_run_with_secret(db,b'legacy',cfg.encode(),b'READ_SOURCE',None,b''));assert result['status']=='completed',result
    finally:lib.dc_close(db)
    raise SystemExit(0)
if '--read-restored' in sys.argv:
    db=lib.dc_open(sys.argv[3].encode());assert db
    try:
        assert value(lib.dc_messages(db,b'legacy'))
        assert lib.dc_rename_chat(db,b'legacy',b'Restored title')
        with sqlite3.connect(pathlib.Path(sys.argv[3])/'conversations.sqlite') as sql:raw=sql.execute("SELECT state FROM chats WHERE id='legacy'").fetchone()[0]
        print(hashlib.sha256(raw.encode()).hexdigest(),flush=True)
    finally:lib.dc_close(db)
    raise SystemExit(0)
prior_image=str(pathlib.Path(sys.argv[2]).resolve()) if len(sys.argv)>2 else None
seen=[];errors=[]
class Model(http.server.BaseHTTPRequestHandler):
    def log_message(self,*_):pass
    def do_POST(self):
        try:
            body=json.loads(self.rfile.read(int(self.headers['Content-Length'])));seen.append(body)
            messages=body['messages'];last_user=max(i for i,m in enumerate(messages) if m['role']=='user');tail=messages[last_user+1:]
            if not any(m['role']=='tool' for m in tail):
                message=dict(role='assistant',content=None,reasoning_content='Public fixture thought',tool_calls=[dict(id='read-'+str(len(seen)),type='function',function=dict(name='read_file',arguments='{"path":"source.txt"}'))]);finish='tool_calls'
            else:message=dict(role='assistant',content='Read the owned source.',reasoning_content='Public completed thought');finish='stop'
            data=enc(dict(choices=[dict(index=0,message=message,finish_reason=finish)]))
            self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
        except BaseException as e:errors.append(repr(e))
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='dunlin-chat-workspace-') as tmp:
    root=pathlib.Path(tmp);state=root/'state';state.mkdir(mode=0o700);a=root/'A';a.mkdir();b=root/'B';b.mkdir()
    (a/'source.txt').write_text('ORIGINAL_A_SOURCE');(b/'source.txt').write_text('DIFFERENT_B_SOURCE')
    db=lib.dc_open(str(state).encode());assert db
    def request(op,**kw):return value(lib.dc_workspace_request(db,enc(dict(op=op,**kw))))
    def ok(op,**kw):
        r=request(op,**kw);assert r['ok'],r;return r.get('data')
    try:
        project=ok('projects.create',id='p',name='Owned project',path=str(a),instructions='',model='',policy='ask',context=['source.txt'])
        assert ok('chats.workspace.migration')['kind']=='initial_format'
        assert lib.dc_add_chat(db,b'old',b'Original chat',str(a).encode())
        assert lib.dc_add_chat(db,b'model-old',b'Original model chat',str(a).encode())
        cfg=dict(base_url=f'http://127.0.0.1:{server.server_port}/v1',model='owned-fixture',protocol='chat',stream=False,desktop_tools='ask',desktop_auth='none',desktop_project_context=[])
        def run(chat,selection=None):
            settings=dict(cfg,desktop_project_context=selection or [])
            return value(lib.dc_run_with_secret(db,chat.encode(),enc(settings),b'READ_SOURCE',None,b''))
        assert run('model-old')['status']=='completed' and not errors
        with fixture_sqlite.connect(state/'conversations.sqlite') as sql:
            source=json.dumps(dict(version=2,active=False,rounds=0,messages=[dict(role='user',content='Original exact user source')]))
            sql.execute("UPDATE chats SET state=? WHERE id='old'",(source,))
        project=ok('projects.update',**dict(project,path=str(b)),relink=True)
        old=next(c for c in value(lib.dc_chats(db)) if c['id']=='old')
        assert old['workspace']==str(a.resolve()),('different project default silently reassigned existing chat',old)
        with fixture_sqlite.connect(state/'conversations.sqlite') as sql:assert sql.execute("SELECT state FROM chats WHERE id='old'").fetchone()[0]==source
        assert lib.dc_add_chat(db,b'new',b'New chat',str(b).encode())
        assert next(c for c in value(lib.dc_chats(db)) if c['id']=='new')['workspace']==str(b.resolve())
        assert ok('files.read',project='p',path='source.txt')['text']=='DIFFERENT_B_SOURCE'
        assert ok('files.read',project='p',chat='old',path='source.txt')['text']=='ORIGINAL_A_SOURCE'
        assert ok('files.read',project='p',chat='new',path='source.txt')['text']=='DIFFERENT_B_SOURCE'
        print('PASS project default change preserves original chat root/source; new chats use new default and scoped file reads stay separate',flush=True)
        selected=[dict(path='source.txt',sha256=hashlib.sha256((a/'source.txt').read_bytes()).hexdigest())]
        assert run('model-old',selected)['status']=='completed',errors
        assert 'ORIGINAL_A_SOURCE' in json.dumps(seen[-1]) and 'DIFFERENT_B_SOURCE' not in json.dumps(seen[-1])
        assert run('new')['status']=='completed'
        assert 'DIFFERENT_B_SOURCE' in json.dumps(seen[-1]) and 'ORIGINAL_A_SOURCE' not in json.dumps(seen[-1])
        for invalid in [dict(project='p',chat='absent'),dict(project='absent',chat='model-old'),dict(project='p',chat=None),dict(project='p',chat=1)]:
            assert not request('files.read',path='source.txt',**invalid)['ok']
        offline_b=root/'offline-B';b.rename(offline_b)
        assert not ok('projects.list')[0]['available']
        assert run('model-old',selected)['status']=='completed','default folder outage blocked independent old workspace'
        count=len(seen);assert run('new')['status']=='failed' and len(seen)==count
        project=ok('projects.list')[0];project=ok('projects.update',**dict(project,path=str(offline_b)),relink=True)
        assert next(c for c in value(lib.dc_chats(db)) if c['id']=='new')['workspace']==str(offline_b.resolve())
        assert next(c for c in value(lib.dc_chats(db)) if c['id']=='model-old')['workspace']==str(a.resolve())
        assert run('new')['status']=='completed'
        print('PASS actual model/read tool/reference content remains scoped; unrelated default outage does not block old root; same physical relocation updates only matching chats',flush=True)
        original_a=root/'original-A';a.rename(original_a);a.mkdir();(a/'source.txt').write_text('UNAUTHORIZED_REPLACEMENT')
        old_source=value(lib.dc_messages(db,b'model-old'));count=len(seen)
        assert run('model-old')['status']=='failed' and len(seen)==count
        assert value(lib.dc_messages(db,b'model-old'))==old_source
        assert not request('files.read',project='p',chat='model-old',path='source.txt')['ok']
        def review(chat='model-old',path=original_a):return ok('chats.workspace.preview',chat=chat,path=str(path))
        def apply(plan):
            fields=['chat','path','device','inode','project','bindingRevision','projectRevision','historyHash']
            return request('chats.workspace.apply',**{k:plan[k] for k in fields})
        plan=review();assert plan['canApply'] and plan['messageCount']>0
        read=value(lib.dc_read_view(str(state).encode(),enc(dict(op='chats.workspace.preview',chat='model-old',path=str(original_a)))));assert read['ok'] and read['data']==plan
        assert len(seen)==count
        with fixture_sqlite.connect(state/'conversations.sqlite') as sql:
            prior=sql.execute("SELECT state FROM chats WHERE id='model-old'").fetchone()[0]
            active=json.loads(prior);active['active']=True;sql.execute("UPDATE chats SET state=? WHERE id='model-old'",(json.dumps(active),))
        assert not apply(plan)['ok'] and not review()['canApply']
        with fixture_sqlite.connect(state/'conversations.sqlite') as sql:sql.execute("UPDATE chats SET state=? WHERE id='model-old'",(prior,))
        bad=dict(plan,bindingRevision=float(plan['bindingRevision']));assert not apply(bad)['ok']
        project=ok('projects.update',**dict(project,name='Updated project name'))
        assert not apply(plan)['ok'];plan=review()
        with fixture_sqlite.connect(state/'conversations.sqlite') as sql:
            sql.execute("CREATE TRIGGER fail_binding BEFORE UPDATE ON chat_workspace_bindings BEGIN SELECT RAISE(ABORT,'owned binding failure'); END")
        assert not apply(plan)['ok']
        assert next(c for c in value(lib.dc_chats(db)) if c['id']=='model-old')['workspace']==str(a.resolve())
        with fixture_sqlite.connect(state/'conversations.sqlite') as sql:
            assert sql.execute("SELECT state FROM chats WHERE id='model-old'").fetchone()[0]==prior
            sql.execute('DROP TRIGGER fail_binding')
        assert apply(plan)['ok'];assert not apply(plan)['ok'],'a workspace review must be one-use'
        assert len(seen)==count and value(lib.dc_messages(db,b'model-old'))==old_source
        assert ok('projects.list')[0]['path']==str(offline_b.resolve()),'chat relink changed default project'
        assert ok('files.read',project='p',chat='model-old',path='source.txt')['text']=='ORIGINAL_A_SOURCE'
        assert run('model-old',selected)['status']=='completed'
        assert 'UNAUTHORIZED_REPLACEMENT' not in json.dumps(seen[-1])
        assert (a/'source.txt').read_text()=='UNAUTHORIZED_REPLACEMENT'
        print('PASS actual replacement blocks before HTTP; source remains readable; explicit chat-only review is readonly, typed/CAS/project bound, active/SQL-fault safe and one-use',flush=True)
        if prior_image:
            with fixture_sqlite.connect(state/'conversations.sqlite') as sql:prior_source=sql.execute("SELECT state FROM chats WHERE id='model-old'").fetchone()[0]
            count=len(seen);lib.dc_close(db);db=None
            child=subprocess.run([sys.executable,'-B',__file__,prior_image,'--old-child',str(state),json.dumps(cfg)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=8)
            assert child.returncode==0,(child.stdout,child.stderr)
            old=json.loads(child.stdout);assert not old['opened'] or old['result']['status']!='completed',old
            assert len(seen)==count,'old desktop sent a model request before guarded admission'
            db=lib.dc_open(str(state).encode());assert db
            with fixture_sqlite.connect(state/'conversations.sqlite') as sql:assert sql.execute("SELECT state FROM chats WHERE id='model-old'").fetchone()[0]==prior_source
            assert run('model-old',selected)['status']=='completed'
            legacy_state=root/'legacy-state';legacy_state.mkdir(mode=0o700);legacy_work=root/'legacy-work';legacy_work.mkdir();(legacy_work/'source.txt').write_text('LEGACY_ORIGINAL_SOURCE')
            child=subprocess.run([sys.executable,'-B',__file__,prior_image,'--legacy-create',str(legacy_state),str(legacy_work),json.dumps(cfg)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=8)
            assert child.returncode==0,(child.stdout,child.stderr)
            with sqlite3.connect(legacy_state/'conversations.sqlite') as sql:legacy_source=sql.execute("SELECT state FROM chats WHERE id='legacy'").fetchone()[0]
            original_legacy=root/'original-legacy';legacy_work.rename(original_legacy);legacy_work.mkdir();(legacy_work/'source.txt').write_text('MUST_NOT_ADOPT_REPLACEMENT')
            # Deterministic regression for SQLite NOFOLLOW versus legitimate
            # parent aliases (including macOS /var -> /private/var).
            legacy_alias=root/'legacy-state-alias';legacy_alias.symlink_to(legacy_state,target_is_directory=True)
            legacy=lib.dc_open(str(legacy_alias).encode());assert legacy
            try:
                row=value(lib.dc_chats(legacy))[0];assert row['workspaceOrigin']=='legacy_project_identity' and not row['workspaceAvailable'],row
                migration=value(lib.dc_workspace_request(legacy,enc(dict(op='chats.workspace.migration'))));assert migration['ok'],migration
                migration=migration['data'];assert migration['kind']=='upgrade_backup' and migration['backupExists'] and migration['backupBytes']>0
                backup=legacy_state/migration['backup'];assert backup.is_file() and backup.stat().st_mode&0o077==0 and backup.stat().st_nlink==1
                with sqlite3.connect(backup) as sql:
                    assert sql.execute('PRAGMA integrity_check').fetchone()[0]=='ok'
                    assert sql.execute("SELECT state FROM chats WHERE id='legacy'").fetchone()[0]==legacy_source
                restored=root/'restored-before-upgrade';restored.mkdir(mode=0o700);shutil.copy2(backup,restored/'conversations.sqlite')
                restored_child=subprocess.run([sys.executable,'-B',__file__,prior_image,'--read-restored',str(restored)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=8)
                assert restored_child.returncode==0,(restored_child.stdout,restored_child.stderr)
                assert restored_child.stdout.strip()==hashlib.sha256(legacy_source.encode()).hexdigest()
                count=len(seen);result=value(lib.dc_run_with_secret(legacy,b'legacy',enc(cfg),b'READ_SOURCE',None,b''));assert result['status']=='failed' and len(seen)==count
                with fixture_sqlite.connect(legacy_state/'conversations.sqlite') as sql:assert sql.execute("SELECT state FROM chats WHERE id='legacy'").fetchone()[0]==legacy_source
            finally:lib.dc_close(legacy)
            print('PASS actual prior-image process cannot admit a new run on guarded data; current writer reopens; legacy project migration retains declared identity and rejects a replacement without altering source',flush=True)
            blocked_state=root/'backup-blocked-state';blocked_state.mkdir(mode=0o700)
            child=subprocess.run([sys.executable,'-B',__file__,prior_image,'--legacy-create',str(blocked_state),str(original_legacy),json.dumps(cfg)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=8)
            assert child.returncode==0,(child.stdout,child.stderr)
            with sqlite3.connect(blocked_state/'conversations.sqlite') as sql:blocked_source=sql.execute("SELECT state FROM chats WHERE id='legacy'").fetchone()[0]
            outside=root/'not-a-private-backup-directory';outside.mkdir();(blocked_state/'Backups').symlink_to(outside,target_is_directory=True)
            count=len(seen);blocked=lib.dc_open(str(blocked_state).encode());assert not blocked and len(seen)==count
            with sqlite3.connect(blocked_state/'conversations.sqlite') as sql:assert sql.execute("SELECT state FROM chats WHERE id='legacy'").fetchone()[0]==blocked_source
            assert list(outside.iterdir())==[]
            print('PASS complete private pre-upgrade SQLite backup, independent old-image restore/source equality, and unsafe-backup-path failure preserving legacy source',flush=True)
        assert not errors,errors
    finally:
        if db:lib.dc_close(db)
server.shutdown();server.server_close()
