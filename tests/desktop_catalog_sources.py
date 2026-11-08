"""Owned source JSON persistence, manual reload and failure boundaries; no network."""
import ctypes as C,json,pathlib,sys,tempfile,sqlite3,os
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_catalog_call',[P,S,P],P)]:
 f=getattr(lib,n);f.argtypes=a;f.restype=r
with tempfile.TemporaryDirectory(prefix='dunlin-sources-') as tmp:
 root=pathlib.Path(tmp);home=root/'owned';home.mkdir(mode=0o700);db=lib.dc_open(str(home).encode());assert db
 def req(op,**kw):
  p=lib.dc_catalog_call(db,json.dumps(dict(op=op,**kw)).encode(),None);assert p
  try:return json.loads(C.string_at(p))
  finally:lib.dc_free(p)
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r.get('data')
 assert ok('catalog.list')==[]
 path=home/'catalog-sources.json';assert pathlib.Path(ok('catalog.config')['path'])==path
 assert json.loads(path.read_text())==dict(protocol='dunlin.catalog.sources',major=1,sources=[])
 assert path.stat().st_mode&0o777==0o600
 source=dict(id='owned',title='Owned fixture',url='https://catalog.invalid/owned.json',publicKey='11'*32)
 ok('catalog.add',**source);saved=json.loads(path.read_text());assert saved['sources']==[dict(source,enabled=True)]
 ok('catalog.rename',id='owned',title='Renamed');assert json.loads(path.read_text())['sources'][0]['title']=='Renamed'
 ok('catalog.remove',id='owned');assert not json.loads(path.read_text())['sources'][0]['enabled']
 ok('catalog.restore',id='owned');assert json.loads(path.read_text())['sources'][0]['enabled']
 manual=json.loads(path.read_text());manual['sources'].append(dict(source,id='second',title='Manual',url='https://catalog.invalid/second.json',enabled=True));path.write_text(json.dumps(manual))
 assert len(ok('catalog.list'))==2
 lib.dc_close(db);db=lib.dc_open(str(home).encode());assert len(ok('catalog.list'))==2
 canonical=path.read_bytes()
 for bad in [b'{broken',json.dumps(dict(manual,major=2)).encode(),json.dumps(dict(manual,sources=[manual['sources'][0]]*2)).encode(),b' '*262145]:
  path.write_bytes(bad);assert not req('catalog.list')['ok'];assert path.read_bytes()==bad
 path.write_bytes(canonical)
 changed=json.loads(canonical);changed['sources'][0]['publicKey']='22'*32;path.write_text(json.dumps(changed));assert req('catalog.list')['error']=='source_identity_changed'
 path.write_bytes(canonical);assert ok('catalog.list')[0]['publicKey']=='11'*32
 # A bad file must not mutate trusted cache rows or affect unrelated chats.
 assert all(r['enabled'] for r in ok('catalog.list'))
 path.unlink();target=root/'outside.json';target.write_bytes(canonical);path.symlink_to(target);assert req('catalog.list')['error']=='source_config_unreadable';assert target.read_bytes()==canonical;path.unlink();path.write_bytes(canonical)
 # Missing entries disable rather than deleting signed package/history records.
 minimal=json.loads(canonical);minimal['sources']=minimal['sources'][1:];path.write_text(json.dumps(minimal));rows=ok('catalog.list');assert not next(r for r in rows if r['id']=='owned')['enabled']
 # Publication failure before rename retains the old file and DB state.
 with sqlite3.connect(home/'conversations.sqlite') as sql:
  sql.execute("CREATE TRIGGER reject_source_add BEFORE INSERT ON catalog_sources WHEN NEW.id='rejected' BEGIN SELECT RAISE(ABORT,'owned failure'); END")
 before=path.read_bytes();assert not req('catalog.add',**dict(source,id='rejected',url='https://catalog.invalid/rejected.json'))['ok'];assert path.read_bytes()==before
 assert not any(r['id']=='rejected' for r in ok('catalog.list'))
 # Embedded NUL/unknown field/invalid boolean cannot truncate or coerce trust data.
 for field,value in [('id','bad\0suffix'),('enabled','yes'),('publicKey','11'*32+'\0suffix'),('unknown',1)]:
  bad=json.loads(canonical);bad['sources'][0][field]=value;path.write_text(json.dumps(bad));assert req('catalog.list')['error']=='source_config_invalid'
 path.write_bytes(canonical);assert len(ok('catalog.list'))==2;lib.dc_close(db)
 print('PASS empty local config, client add/rename/disable/restore, manual reload/restart, immutable URL/key, malformed/oversized/symlink/NUL rejection, rollback and missing-source disable; no network')
