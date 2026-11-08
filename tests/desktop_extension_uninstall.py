"""Uninstall stops real worker effects, clears all versions, retains fault evidence."""
import ctypes as C,json,pathlib,sys,tempfile,sqlite3
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P)]:
 f=getattr(lib,n);f.argtypes=a;f.restype=r
with tempfile.TemporaryDirectory(prefix='dunlin-uninstall-') as tmp:
 home=pathlib.Path(tmp);state=home/'data';state.mkdir(mode=0o700);db=lib.dc_open(str(state).encode());assert db
 def req(op,**kw):
  raw=lib.dc_extensions_call(db,json.dumps(dict(op=op,**kw)).encode(),None);assert raw
  try:return json.loads(C.string_at(raw))
  finally:lib.dc_free(raw)
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r.get('data')
 ok('extensions.assets',path=str(pathlib.Path(sys.argv[2]).resolve()));ok('python.configure',executable=sys.executable)
 package=home/'package';package.mkdir();m=dict(protocol='dunlin.extension',major=1,packageId='uninstall',version='1.0.0',kind='plugin',engines={'dunlin':'desktop@1','host':'python'},entry='main.py',capabilities=['trusted-full-host'],requires=[],dependencies=[])
 (package/'main.py').write_text("def activate(api,config):\n api.register_command('read',lambda args: {'ok':True})\n api.register_command('fail',lambda args: 1/0)\n")
 for version in ['1.0.0','2.0.0']:
  m['version']=version;(package/'dunlin.json').write_text(json.dumps(m));row=ok('extensions.import',path=str(package))[0]
 ref={k:row[k] for k in ['source','package']};assert len(row['versions'])==2
 ok('extensions.bind',**ref,version='2.0.0',enabled=True,acceptHostPermissions=True)
 assert ok('extensions.command',**ref,command='read',arguments={})['data']['ok']
 assert not req('extensions.command',**ref,command='fail',arguments={})['ok'];faults=ok('extensions.faults');assert faults
 assert ok('extensions.uninstall',**ref)==[]
 assert ok('extensions.faults')==faults
 assert not req('extensions.command',**ref,command='read',arguments={})['ok']
 with sqlite3.connect(state/'conversations.sqlite') as sql:
  for table in ['extension_versions','extension_bindings','extension_configuration']:assert sql.execute('SELECT count(*) FROM '+table).fetchone()[0]==0
 lib.dc_close(db);db=lib.dc_open(str(state).encode());assert ok('extensions.list')==[];assert ok('extensions.faults')==faults
 assert not req('extensions.uninstall',source='missing',package='missing')['ok'];lib.dc_close(db)
 print('PASS two-version import/update, real Python execution, fault quarantine, uninstall all versions/config, retained logs and restart non-execution')
