"""Actual npm metadata/artifact, C TLS/integrity/extraction, Node host execution."""
import ctypes as C,json,pathlib,sys,tempfile
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P),('dc_catalog_call',[P,S,P],P)]:
 f=getattr(lib,n);f.argtypes=a;f.restype=r
with tempfile.TemporaryDirectory(prefix='dunlin-npm-') as tmp:
 db=lib.dc_open(tmp.encode());assert db
 def req(op,**kw):
  fn=lib.dc_catalog_call if op.startswith('catalog.') else lib.dc_extensions_call
  raw=fn(db,json.dumps(dict(op=op,**kw)).encode(),None);assert raw
  try:return json.loads(C.string_at(raw))
  finally:lib.dc_free(raw)
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r.get('data')
 published=ok('catalog.npm.versions',repository='https://github.com/zhijun-dai/Catppuccin-dsh-theme');assert '0.2.6' in published and '0.2.3' in published and '0.2.5' not in published
 source=dict(repository='https://github.com/zhijun-dai/Catppuccin-dsh-theme',version='0.2.6')
 assert req('catalog.npm.install',**source)['error']=='dsh_environment_not_ready'
 ok('extensions.assets',path=str(pathlib.Path(sys.argv[2]).resolve()));ok('compat.enable',node='/opt/homebrew/bin/node')
 for bad in [dict(source,version='../latest'),dict(source,repository='https://example.invalid/unknown')]:assert req('catalog.npm.install',**bad)['error']=='npm_package_or_version_unsupported'
 old=ok('catalog.npm.install',**dict(source,version='0.2.3'));assert any(v['version']=='0.2.3+dunlin.2' for v in old[0]['versions'])
 rows=ok('catalog.npm.install',**source);row=next(r for r in rows if r['package']=='dsh-catppuccin');assert not row['enabled'];ref={k:row[k] for k in ['source','package','version']};ref['version']='0.2.6+dunlin.2'
 assert len(row['versions'])==2
 ok('extensions.bind',**dict(ref,version='0.2.3+dunlin.2'),enabled=True,acceptHostPermissions=True)
 ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)
 route=dict(source=ref['source'],package=ref['package'],command='route:/catppuccin/state')
 assert ok('extensions.command',**route,arguments={'method':'PUT','body':{'flavor':'latte'}})['data']['status']==200
 assert ok('extensions.command',**route,arguments={'method':'GET'})['data']['body']['flavor']=='latte'
 assert ok('extensions.uninstall',source=ref['source'],package=ref['package'])==[]
 assert not req('extensions.command',**route,arguments={})['ok']
 assert not list((pathlib.Path(tmp)/'extension-store').iterdir())
 lib.dc_close(db)
 print('PASS C npm real HTTPS metadata/SHA512/gzip/tar install default-off, real upstream Node state effects, uninstall/cache cleanup and denied execution')
