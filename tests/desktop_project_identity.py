#!/usr/bin/env python3
"""Project identity/relink source guards. No model, tools, credentials or approval."""
import desktop_fixture_sqlite as fixture_sqlite
import ctypes as C,hashlib,json,pathlib,sqlite3,sys,tempfile
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for name,args,ret in [('dc_open',[S],P),('dc_close',[P],None),('dc_workspace_request',[P,S],P),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_chats',[P],P)]:
    f=getattr(lib,name);f.argtypes=args;f.restype=ret
def value(p):
    assert p
    try:return json.loads(C.string_at(p))
    finally:lib.dc_free(p)
failures=[]
def check_case(case,action):
    try:action();print('PASS',case,flush=True)
    except AssertionError as e:failures.append((case,str(e)));print('FAIL',case,str(e),flush=True)
with tempfile.TemporaryDirectory(prefix='dunlin-project-identity-') as tmp:
    base=pathlib.Path(tmp)
    def fixture(label,edit,mutate=None,accepted=False):
        parent=base/label;parent.mkdir();root=parent/'project';root.mkdir();(root/'source.txt').write_text('original owned source\n')
        state=parent/'state';state.mkdir(mode=0o700);db=lib.dc_open(str(state).encode());assert db
        sql=fixture_sqlite.connect(state/'conversations.sqlite')
        def request(op,**kw):return value(lib.dc_workspace_request(db,json.dumps(dict(op=op,**kw)).encode()))
        def ok(op,**kw):
            r=request(op,**kw);assert r['ok'],r;return r.get('data')
        try:
            project=ok('projects.create',id='p',name='Owned',path=str(root),instructions='',model='',policy='ask',context=['source.txt'])
            assert lib.dc_add_chat(db,b'chat',b'Owned chat',str(root).encode())
            digest=hashlib.sha256((root/'source.txt').read_bytes()).hexdigest()
            project=ok('projects.update',**project,contextVersions={'source.txt':digest})
            sql.execute("UPDATE chats SET state=? WHERE id='chat'",(json.dumps(dict(version=2,active=False,messages=[dict(role='user',content='Keep this exact history')],rounds=0)),));sql.commit()
            original_state=sql.execute("SELECT state FROM chats WHERE id='chat'").fetchone()[0]
            if mutate:mutate(sql,root)
            project=ok('projects.list')[0]
            before_chat=value(lib.dc_chats(db))[0]
            before=sql.execute('SELECT * FROM projects').fetchall();baseline=sql.execute('SELECT * FROM project_reference_baselines').fetchall()
            update=dict(project);update.update(edit(project,root))
            result=request('projects.update',**update)
            if accepted:
                assert result['ok'],result
                assert result['data']['available'],result
                assert result['data']['revision']==project['revision']+1
                assert sql.execute("SELECT state FROM chats WHERE id='chat'").fetchone()[0]==original_state
                # Approved page 35: adopting a different physical default is
                # for new chats. Relocate old chats only when identity matches.
                same_identity=(before_chat['workspaceDevice'],before_chat['workspaceInode'])==(result['data']['device'],result['data']['inode'])
                assert value(lib.dc_chats(db))[0]['workspace']==(result['data']['path'] if same_identity else before_chat['workspace'])
                return result['data'],before,baseline,sql.execute('SELECT * FROM project_reference_baselines').fetchall()
            assert not result['ok'],('unexpected accepted mutation',result)
            assert sql.execute('SELECT * FROM projects').fetchall()==before,'failed update changed project'
            assert sql.execute('SELECT * FROM project_reference_baselines').fetchall()==baseline,'failed update erased reference baseline'
            assert sql.execute("SELECT state FROM chats WHERE id='chat'").fetchone()[0]==original_state
            assert all(p.read_text() in ('original owned source\n','new owned source\n') for p in parent.rglob('source.txt'))
        finally:sql.close();lib.dc_close(db)
    def device_mismatch(sql,root):
        # A persisted identity from a different volume, not a claim that this
        # test actually remounted a physical volume.
        sql.execute('UPDATE projects SET device=?',(str(root.stat().st_dev+1),));sql.commit()
    for label,suffix in [('same-device-mismatch',''),('alias-device-mismatch','/.')]:
        check_case(label,lambda label=label,suffix=suffix:fixture(label,lambda p,r:dict(path=str(r)+suffix,name='Rename only'),device_mismatch))
    for i,bad in enumerate(['false',0,1,None,{},[]]):
        check_case('relink-type-'+str(i),lambda i=i,bad=bad:fixture('relink-type-'+str(i),lambda p,r:dict(relink=bad)))
    for i,bad in enumerate([float(2),str(2),True,None,0,18446744073709551615]):
        check_case('revision-type-'+str(i),lambda i=i,bad=bad:fixture('revision-type-'+str(i),lambda p,r:dict(revision=bad)))
    def replaced(sql,root):
        root.rename(root.with_name('original-project'));root.mkdir();(root/'source.txt').write_text('new owned source\n')
    check_case('actual-directory-replacement',lambda:fixture('actual-directory-replacement',lambda p,r:dict(name='Rename only'),replaced))
    def symlink_replaced(sql,root):
        root.rename(root.with_name('original-project'));new=root.with_name('replacement');new.mkdir();(new/'source.txt').write_text('new owned source\n');root.symlink_to(new,target_is_directory=True)
    check_case('same-path-symlink-replacement',lambda:fixture('same-path-symlink-replacement',lambda p,r:dict(name='Rename only'),symlink_replaced))
    def explicit_relink():
        p,_,baseline,after=fixture('explicit-relink',lambda p,r:dict(relink=True),device_mismatch,True)
        assert p['available'] and baseline and not after
    check_case('explicit-boolean-relink',explicit_relink)
    def alias_without_identity_change():
        p,_,baseline,after=fixture('unchanged-alias',lambda p,r:dict(path=str(r)+'/.',name='Renamed'),accepted=True)
        assert p['available'] and baseline==after and baseline,'canonical alias must not erase acknowledged versions'
    check_case('valid-alias-preserves-baseline',alias_without_identity_change)
    def explicit_different_path():
        def edit(p,r):
            new=r.with_name('selected-project');new.mkdir();(new/'source.txt').write_text('new owned source\n');return dict(path=str(new))
        p,_,baseline,after=fixture('explicit-new-path',edit,accepted=True)
        assert p['path'].endswith('/selected-project') and baseline and not after
    check_case('explicit-different-canonical-path',explicit_different_path)
    def explicit_replaced_directory():
        p,_,baseline,after=fixture('explicit-replaced-directory',lambda p,r:dict(relink=True),replaced,True)
        assert p['available'] and baseline and not after
    check_case('explicit-replaced-directory',explicit_replaced_directory)
if failures:raise AssertionError(failures)
print('PASS project directory identity, typed intent/revision and failed-change source preservation; no model request or tool effects',flush=True)
