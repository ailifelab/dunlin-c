#!/usr/bin/env python3
"""Real owned Python activation effects, audit failures, interrupted recovery."""
import ctypes as C,json,sys,pathlib,tempfile,sqlite3
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P)]:
 f=getattr(lib,n);f.argtypes=a;f.restype=r
with tempfile.TemporaryDirectory(prefix='dunlin-lifecycle-') as tmp:
 root=pathlib.Path(tmp);private=root/'private';private.mkdir(mode=0o700);assets=pathlib.Path(sys.argv[2]).resolve();db=lib.dc_open(str(private).encode());assert db
 def req(op,**kw):
  raw=lib.dc_extensions_call(db,json.dumps(dict(op=op,**kw)).encode(),None);assert raw
  try:return json.loads(C.string_at(raw))
  finally:lib.dc_free(raw)
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r.get('data')
 ok('extensions.assets',path=str(assets));ok('python.configure',executable=sys.executable)
 plugin=root/'plugin';plugin.mkdir();effect=root/'effect'
 m={'protocol':'dunlin.extension','major':1,'packageId':'lifecycle','version':'1.0.0','displayName':'Owned lifecycle','kind':'plugin','engines':{'dunlin':'desktop@1','host':'python'},'entry':'main.py','capabilities':['trusted-full-host'],'requires':['dunlin.contributions@1'],'dependencies':[]}
 (plugin/'dunlin.json').write_text(json.dumps(m));(plugin/'main.py').write_text("from pathlib import Path\ndef activate(api,config):\n p=Path("+repr(str(effect))+");p.write_text(p.read_text()+'x' if p.exists() else 'x')\n api.register_command('read',lambda args: {'ok':True})\n")
 row=ok('extensions.import',path=str(plugin))[0];ref={k:row[k] for k in ['source','package','version']}
 audit=sqlite3.connect(private/'conversations.sqlite')
 audit.execute("CREATE TRIGGER deny_lifecycle BEFORE INSERT ON extension_calls BEGIN SELECT RAISE(ABORT,'owned audit denial'); END");audit.commit()
 result=req('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)
 assert not result['ok'] and not effect.exists(),('activation ran without durable audit',result)
 audit.execute('DROP TRIGGER deny_lifecycle');audit.commit()
 ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True);assert effect.read_text()=='x'
 assert audit.execute("SELECT status FROM extension_calls WHERE method='plugins.bind'").fetchall()==[('reported',)]
 # Post-effect audit failure must leave a durable unknown boundary and stop host.
 audit.execute("CREATE TRIGGER deny_finish BEFORE UPDATE ON extension_calls WHEN NEW.status='reported' BEGIN SELECT RAISE(ABORT,'owned finalization denial'); END");audit.commit()
 result=req('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)
 assert not result['ok'] and effect.read_text()=='xx',result
 assert not ok('extensions.list')[0]['active']
 audit.execute('DROP TRIGGER deny_finish');audit.commit();audit.close();lib.dc_close(db)
 db=lib.dc_open(str(private).encode());assert db;ok('extensions.assets',path=str(assets))
 assert ok('extensions.restore')['restored']==0 and effect.read_text()=='xx'
 assert not req('extensions.recover',**ref)['ok'],'unknown activation must require reconciliation'
 audit=sqlite3.connect(private/'conversations.sqlite');assert audit.execute("SELECT count(*) FROM extension_calls WHERE status='unknown'").fetchone()[0]==1;audit.close()
 ok('python.configure',executable=sys.executable)
 crash=root/'host-exit';crash.mkdir();cm=dict(m,packageId='host-exit');(crash/'dunlin.json').write_text(json.dumps(cm))
 (crash/'main.py').write_text('import os\ndef activate(api,config):\n os._exit(45)\n')
 cr=next(x for x in ok('extensions.import',path=str(crash)) if x['package']=='host-exit');cr={k:cr[k] for k in ['source','package','version']}
 assert not req('extensions.bind',**cr,enabled=True,acceptHostPermissions=True)['ok']
 audit=sqlite3.connect(private/'conversations.sqlite')
 assert audit.execute("SELECT status FROM extension_calls WHERE package='host-exit'").fetchall()==[('unknown',)],'transport error must remain unknown'
 audit.close();lib.dc_close(db)
 print('PASS lifecycle audit denial prevents effects; successful activation finalizes; post-effect audit failure quarantines and cannot replay on restart')
