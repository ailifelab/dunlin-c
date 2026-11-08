"""Actual pinned upstream entrypoints; isolated host/home, never live model credentials."""
import ctypes as C,json,pathlib,sys,tempfile,sqlite3
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P)]:
 f=getattr(lib,n);f.argtypes=a;f.restype=r
inputs=json.load(open(sys.argv[3]));results=[]
with tempfile.TemporaryDirectory(prefix='dunlin-real-dsh-') as tmp:
 home=pathlib.Path(tmp);db=lib.dc_open(str(home).encode());assert db
 def req(op,**kw):
  raw=lib.dc_extensions_call(db,json.dumps(dict(op=op,**kw)).encode(),None);assert raw
  try:return json.loads(C.string_at(raw))
  finally:lib.dc_free(raw)
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r.get('data')
 ok('extensions.assets',path=str(pathlib.Path(sys.argv[2]).resolve()));ok('compat.enable',node='/opt/homebrew/bin/node')
 for item in inputs:
  response=req('extensions.import',path=str(pathlib.Path(item['path']).resolve()))
  result={k:item[k] for k in ['repository','commit','name','version']};result['import']=response['ok']
  if not response['ok']:result['error']=response['error'];results.append(result);continue
  row=next(r for r in response['data'] if r['package']==item['name']);ref={k:row[k] for k in ['source','package','version']}
  activation=req('extensions.bind',**ref,enabled=True,acceptHostPermissions=True);result['activation']=activation['ok']
  if item['name']=='dsh-catppuccin':
   assert activation['ok'],activation
   contributions=ok('extensions.contributions');assert any(c.get('command')=='route:/catppuccin/state' for c in contributions)
   route=dict(source=ref['source'],package=ref['package'],command='route:/catppuccin/state')
   value=ok('extensions.command',**route,arguments={'method':'PUT','body':{'flavor':'mocha'}});assert value['data']['status']==200,value
   value=ok('extensions.command',**route,arguments={'method':'GET'});assert value['data']['body']['flavor']=='mocha',value
   ok('extensions.bind',**ref,enabled=False);assert not any(c['plugin']==row['runtimeID'] for c in ok('extensions.contributions'))
   ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True);assert ok('extensions.command',**route,arguments={'method':'GET'})['data']['body']['flavor']=='mocha'
   result['realStateRoundTrip']=True
  else:
   assert not activation['ok'],('Unexpected support; verify actual effects before accepting',item['name'])
   current=next(r for r in ok('extensions.list') if r['package']==item['name']);assert not current['enabled'] and not current['active'] and current['versions'][0]['quarantined']
   logs=[f for f in ok('extensions.faults') if f['package']==item['name']];assert logs;result['faults']=logs
  removed=ok('extensions.uninstall',source=ref['source'],package=ref['package']);assert not any(r['package']==item['name'] for r in removed)
  assert not any(c['plugin']==row['runtimeID'] for c in ok('extensions.contributions'))
  assert not req('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)['ok']
  result['uninstall']=True;results.append(result)
 # Uninstall retains forensic failure records and cannot silently retry unknown calls.
 assert ok('extensions.faults');assert ok('extensions.list')==[]
 lib.dc_close(db)
pathlib.Path(sys.argv[4]).write_text(json.dumps(results,indent=2)+'\n')
print('PASS actual upstream activation attempts, Catppuccin persistent JSON route effects, disable/re-enable, unsupported-plugin quarantine/log retention, uninstall prevents execution')
