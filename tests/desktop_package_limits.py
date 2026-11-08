"""Desktop package admission and later integrity verification share one policy."""
import ctypes as C,json,pathlib,sys,tempfile
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P)]:
 f=getattr(lib,n);f.argtypes=a;f.restype=r
with tempfile.TemporaryDirectory(prefix='dunlin-package-bounds-') as tmp:
 home=pathlib.Path(tmp);state=home/'state';state.mkdir(mode=0o700);plugin=home/'plugin';plugin.mkdir();db=lib.dc_open(str(state).encode());assert db
 def req(op,**args):
  ptr=lib.dc_extensions_call(db,json.dumps(dict(op=op,**args)).encode(),None);assert ptr
  try:return json.loads(C.string_at(ptr))
  finally:lib.dc_free(ptr)
 def ok(op,**args):
  r=req(op,**args);assert r['ok'],r;return r['data']
 try:
  ok('extensions.assets',path=str(pathlib.Path(sys.argv[2]).resolve()));ok('python.configure',executable=sys.executable)
  manifest=dict(protocol='dunlin.extension',major=1,packageId='bounds',version='1',kind='plugin',engines={'dunlin':'desktop@1','host':'python'},entry='main.py',capabilities=['trusted-full-host'],requires=[],dependencies=[])
  (plugin/'dunlin.json').write_text(json.dumps(manifest));(plugin/'main.py').write_text("def activate(api,config):\n api.register_command('check',lambda args: {'ok':True})\n")
  for i in range(2046):(plugin/('asset-%04d'%i)).write_bytes(b'owned')
  rows=ok('extensions.import',path=str(plugin));ref={k:rows[0][k] for k in ('source','package','version')};assert ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)[0]['active']
  assert ok('extensions.command',source=ref['source'],package=ref['package'],command='check',arguments={})['data']['ok']
  (plugin/'one-too-many').write_bytes(b'x');assert req('extensions.import',path=str(plugin))['error']=='unsafe_extension_directory'
  (plugin/'one-too-many').unlink();(plugin/'asset-0000').write_bytes(b'x'*(16*1024*1024+1));assert req('extensions.import',path=str(plugin))['error']=='unsafe_extension_directory'
 finally:lib.dc_close(db)
print('PASS 2048-file package imports and executes after integrity recheck; 2049 files and >16 MiB file rejected.')
