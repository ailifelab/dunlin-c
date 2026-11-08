"""Real C index persistence and no-execution contract, with optional live HTTPS."""
import ctypes as C,json,pathlib,sys,tempfile,sqlite3
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_catalog_call',[P,S,P],P)]:
 f=getattr(lib,n);f.argtypes=a;f.restype=r
with tempfile.TemporaryDirectory(prefix='dunlin-community-') as tmp:
 home=pathlib.Path(tmp);db=lib.dc_open(str(home).encode());assert db
 def req(op,**kw):
  p=lib.dc_catalog_call(db,json.dumps(dict(op='catalog.community.'+op,**kw)).encode(),None);assert p
  try:return json.loads(C.string_at(p))
  finally:lib.dc_free(p)
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r['data']
 rows=ok('list');assert len(rows)==2 and all(not r['entries'] for r in rows)
 path=home/'plugin-index-sources.json';assert path.stat().st_mode&0o777==0o600
 ok('add',id='fixture',title='Owned fixture',url='https://example.invalid/index.json')
 assert not req('add',id='duplicate',title='Duplicate',url='https://example.invalid/index.json')['ok']
 assert not req('add',id='bad',title='Bad',url='http://example.invalid/index.json')['ok']
 payload=dict(format=1,plugins=[dict(name='example',title={'zh':'示例'},description={'en':'Example'},repository={'url':'https://github.com/example/example'},install='touch '+str(home/'MUST_NOT_EXIST'),versions=[{'version':'1.0','source':{'type':'command','command':'false'}}])])
 out=ok('import',id='fixture',index=json.dumps(payload));entry=next(r for r in out if r['id']=='fixture')['entries'][0];assert entry==dict(name='example',title='示例',description='Example',url='https://github.com/example/example');assert not (home/'MUST_NOT_EXIST').exists()
 for bad in [dict(format=2,plugins=[]),dict(format=1,plugins={}),dict(format=1,plugins=[dict(name='x',repository={'url':'javascript:alert(1)'})]),dict(format=1,plugins=[dict(name='x\0y',repository={'url':'https://example.org'})])]:
  assert not req('import',id='fixture',index=json.dumps(bad))['ok'];assert next(r for r in ok('list') if r['id']=='fixture')['entries']==[entry]
 lib.dc_close(db);db=lib.dc_open(str(home).encode());assert next(r for r in ok('list') if r['id']=='fixture')['entries']==[entry]
 canonical=path.read_bytes();path.write_text('{');assert not req('list')['ok'];assert path.read_text()=='{';path.write_bytes(canonical)
 external=home/'external';external.write_bytes(canonical);path.unlink();path.symlink_to(external);assert not req('list')['ok'];assert external.read_bytes()==canonical;path.unlink();path.write_bytes(canonical)
 cfg=json.loads(canonical);cfg['sources'][0]['title']='Edited';path.write_text(json.dumps(cfg));assert ok('list')[0]['title']=='Edited'
 ok('remove',id='fixture');assert len(ok('list'))==2
 with sqlite3.connect(home/'conversations.sqlite') as sql:assert sql.execute('SELECT count(*) FROM catalog_packages').fetchone()[0]==0
 if '--live' in sys.argv:
  rows=ok('sync',id='dshseek');assert next(r for r in rows if r['id']=='dshseek')['entries'];print('PASS live DSHSeek HTTPS sync; metadata only, not plugin execution')
 lib.dc_close(db)
 print('PASS defaults/add/remove/manual config, cache/restart, malformed/symlink rejection, last-good cache, no commands/installations')
