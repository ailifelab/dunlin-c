"""Plugin configuration survives manual toggles; no user data or live model."""
import ctypes as C,json,pathlib,sys,tempfile
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
with tempfile.TemporaryDirectory(prefix='dunlin-plugin-config-') as tmp:
 home=pathlib.Path(tmp);state=home/'state';state.mkdir(mode=0o700);package=home/'package';package.mkdir();effect=home/'effect';db=lib.dc_open(str(state).encode());assert db
 def req(op,**kw):
  ptr=lib.dc_extensions_call(db,json.dumps(dict(op=op,**kw)).encode(),None);assert ptr
  try:return json.loads(C.string_at(ptr))
  finally:lib.dc_free(ptr)
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r['data']
 try:
  ok('extensions.assets',path=str(pathlib.Path(sys.argv[2]).resolve()));ok('python.configure',executable=sys.executable)
  (package/'dunlin.json').write_text(json.dumps(dict(protocol='dunlin.extension',major=1,packageId='config-fixture',version='1',kind='plugin',engines={'dunlin':'desktop@1','host':'python'},entry='main.py',capabilities=['trusted-full-host'],requires=[],dependencies=[])))
  (package/'main.py').write_text("from pathlib import Path\ndef activate(api,config):\n def run(args):\n  Path(config['target']).write_text(config['text'])\n  return {'written':True}\n api.register_command('write',run)\n")
  row=ok('extensions.import',path=str(package))[0];ref={k:row[k] for k in ['source','package','version']};config={'target':str(effect),'text':'retained configuration'}
  ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True,config=config);ok('extensions.bind',**ref,enabled=False)
  ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)
  result=ok('extensions.command',source=ref['source'],package=ref['package'],command='write',arguments={});assert result['data']['written'] and effect.read_text()=='retained configuration'
  current=ok('extensions.config.get',**ref);assert current['config']==config and current['active']
  assert req('extensions.config.save',**ref,config={},expectedHash=current['hash'])['error']=='extension_configuration_requires_disable'
  ok('extensions.bind',**ref,enabled=False);next_config=dict(config,text='edited while stopped')
  assert req('extensions.config.save',**ref,config=next_config,expectedHash='stale')['error']=='extension_configuration_changed'
  assert req('extensions.config.save',**ref,config={'large':'x'*32769},expectedHash=current['hash'])['error']=='invalid_extension_configuration'
  saved=ok('extensions.config.save',**ref,config=next_config,expectedHash=current['hash']);assert saved['hash']!=current['hash']
  ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True);ok('extensions.command',source=ref['source'],package=ref['package'],command='write',arguments={});assert effect.read_text()=='edited while stopped'
  lib.dc_close(db);db=lib.dc_open(str(state).encode());assert db;assert ok('extensions.config.get',**ref)['config']==next_config
 finally:lib.dc_close(db)
print('PASS manual disable/re-enable without a new config retains and executes the original persisted configuration.')
