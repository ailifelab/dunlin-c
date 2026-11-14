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
 source=dict(package='dsh-plugin-subscriptions',repository='https://github.com/V1ki/dsh-plugin-subscriptions')
 version='0.9.13'
 assert version in ok('catalog.npm.versions',**source)
 assert req('catalog.npm.install',**source,version=version)['error']=='dsh_environment_not_ready'
 ok('extensions.assets',path=str(pathlib.Path(sys.argv[2]).resolve()))
 ok('compat.enable',node='/opt/homebrew/bin/node')
 mismatch=dict(source,repository='https://github.com/example/not-the-publisher')
 for op in ['catalog.npm.versions','catalog.npm.install']:
  assert req(op,**mismatch,version=version)['error']=='npm_repository_mismatch'
 for name in ['../bad','@scope/../x','a?tag=latest','@/p','a/b']:
  assert req('catalog.npm.install',**dict(source,package=name),version=version)['error']=='npm_package_or_version_unsupported'
 assert ok('extensions.list')==[]
 rows=ok('catalog.npm.install',**source,version=version)
 row=next(r for r in rows if r['package']==source['package'])
 assert not row['active'] and not row['enabled']
 manifest=row['versions'][0]['manifest']
 assert manifest['entry']=='lib/index.js' and manifest['client']['entry']=='lib/client.js' and manifest['repository']==source['repository']
 assert ok('extensions.uninstall',source=row['source'],package=row['package'])==[]
 # Preflight must fail before any module-level code runs, even with user consent.
 root=pathlib.Path(tmp)/'engine-probe';root.mkdir()
 probe=dict(manifest,packageId='engine-probe',entry='server.mjs')
 probe.pop('client');(root/'dunlin.json').write_text(json.dumps(probe))
 (root/'package.json').write_text(json.dumps({'name':'engine-probe','dsh':{'engines':{'dsh':'>=99'}}}))
 marker=pathlib.Path(tmp)/'evaluated'
 (root/'server.mjs').write_text("import fs from 'node:fs'; fs.writeFileSync("+json.dumps(str(marker))+",'unexpected'); export function apply(){}")
 row=ok('extensions.import',path=str(root))[0];ref={k:row[k] for k in ['package','source','version']}
 assert not req('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)['ok']
 assert not marker.exists()
 row=ok('extensions.list')[0];assert not row['active'] and row['versions'][0]['quarantined']
 assert 'Unsupported engine: dsh' in ok('extensions.faults')[0]['diagnostic']
 lib.dc_close(db)
 print('PASS independent community npm package, repository identity, bounded install, React export discovery, default off and uninstall')
