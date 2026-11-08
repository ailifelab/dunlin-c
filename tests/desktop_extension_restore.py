#!/usr/bin/env python3
"""C-owned optional environment and real JS/Python extensions with actual command effects."""
import ctypes as C,json,sys,pathlib,tempfile,sqlite3,hashlib
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
with tempfile.TemporaryDirectory(prefix='dunlin-extensions-') as tmp:
 root=pathlib.Path(tmp);private=root/'private';private.mkdir(mode=0o700);assets=pathlib.Path(sys.argv[2]).resolve();db=lib.dc_open(str(private).encode());assert db
 def req(op,**kw):
  raw=lib.dc_extensions_call(db,json.dumps(dict(op=op,**kw)).encode(),None);assert raw
  try:return json.loads(C.string_at(raw))
  finally:lib.dc_free(raw)
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r.get('data')
 def bad(op,**kw):
  r=req(op,**kw);assert not r['ok'],r;return r
 assert ok('compat.status')['state']=='off';assert not (private/'Compatibility').exists()
 ok('extensions.assets',path=str(assets))

 assert ok('extensions.restore')['restored']==0
 assert not (private/'Compatibility').exists()
 ok('compat.enable',node='/opt/homebrew/bin/node')
 plugin=root/'persist';plugin.mkdir()
 manifest={'protocol':'dunlin.extension','major':1,'packageId':'persist','version':'1.0.0','displayName':'Owned restart fixture','kind':'plugin','engines':{'dunlin':'desktop@1','host':'node-dsh','dsh':'0.2.1-rc.3'},'entry':'index.ts','capabilities':['trusted-full-host'],'requires':['dunlin.contributions@1'],'dependencies':[]}
 (plugin/'dunlin.json').write_text(json.dumps(manifest))
 (plugin/'index.ts').write_text("export const inject=['dunlin'];export function apply(ctx,config){let n=0;ctx.dunlin.registerCommand(ctx,'read',()=>({value:config.label,count:++n}));ctx.dunlin.contribute(ctx,'page',{kind:'sidebar',title:'Restored page',body:'Owned fixture'})}")
 row=ok('extensions.import',path=str(plugin))[0];ref={k:row[k] for k in ['source','package','version']}
 ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True,config={'label':'saved configuration'})
 lib.dc_close(db);db=lib.dc_open(str(private).encode());assert db
 ok('extensions.assets',path=str(assets));report=ok('extensions.restore');assert report['restored']==1 and report['failed']==0,report
 def read():return ok('extensions.command',source=ref['source'],package=ref['package'],command='read',arguments={})['data']
 assert read()=={'value':'saved configuration','count':1}
 assert ok('extensions.restore')['restored']==0
 assert read()['count']==2,'repeat restore reactivated plugin'
 bad('extensions.bind',**ref,enabled=True,acceptHostPermissions=True,config='invalid')
 assert read()['count']==3,'invalid config changed running plugin'
 ok('extensions.bind',**ref,enabled=False)
 lib.dc_close(db);db=lib.dc_open(str(private).encode());assert db
 ok('extensions.assets',path=str(assets));assert ok('extensions.restore')['restored']==0
 assert not ok('extensions.contributions')
 ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True,config={'label':'saved configuration'})
 sql=sqlite3.connect(private/'conversations.sqlite')
 installed=pathlib.Path(sql.execute('SELECT path FROM extension_versions WHERE package=?',('persist',)).fetchone()[0]);sql.close()
 lib.dc_close(db);(installed/'index.ts').write_text('modified after approval')
 db=lib.dc_open(str(private).encode());assert db
 ok('extensions.assets',path=str(assets));report=ok('extensions.restore');assert report['failed']==1,report
 assert ok('extensions.list')[0]['versions'][0]['quarantined']
 assert ok('extensions.restore')['restored']==0
 ok('compat.disable')
 ok('python.configure',executable=sys.executable)
 py=root/'python-restore';py.mkdir();pm=dict(manifest,packageId='python-restore',engines={'dunlin':'desktop@1','host':'python'},entry='main.py')
 (py/'dunlin.json').write_text(json.dumps(pm));(py/'main.py').write_text("def activate(api,config):\n api.register_command('read',lambda args: {'label':config['label']})\n")
 row=next(x for x in ok('extensions.import',path=str(py)) if x['package']=='python-restore');pr={k:row[k] for k in ['source','package','version']}
 ok('extensions.bind',**pr,enabled=True,acceptHostPermissions=True,config={'label':'python saved'})
 lib.dc_close(db);db=lib.dc_open(str(private).encode());assert db
 ok('extensions.assets',path=str(assets));report=ok('extensions.restore');assert report['restored']==1 and report['failed']==0,report
 assert not ok('compat.status')['enabled']
 assert ok('extensions.command',source=pr['source'],package=pr['package'],command='read',arguments={})['data']['label']=='python saved'
 sql=sqlite3.connect(private/'conversations.sqlite');sql.execute("UPDATE compatibility_environment SET desired=1,node=?",(str(root/'missing-engine'),));sql.commit();sql.close()
 lib.dc_close(db);db=lib.dc_open(str(private).encode());assert db
 ok('extensions.assets',path=str(assets));report=ok('extensions.restore')
 assert report['restored']==1 and report['failed']==1 and report['errors']==['dsh_saved_engine_unavailable'],report
 assert ok('python.status')['healthy'] and not ok('compat.status')['healthy']
 assert ok('extensions.command',source=pr['source'],package=pr['package'],command='read',arguments={})['data']['label']=='python saved'
 ok('compat.disable')
 ok('python.disable');lib.dc_close(db)
 db=lib.dc_open(str(private).encode());assert db
 ok('extensions.assets',path=str(assets));report=ok('extensions.restore');assert report['failed']==0 and report['restored']==0,report
 assert not ok('python.status')['healthy']
 assert not ok('extensions.contributions')
 lib.dc_close(db)
 print('PASS invalid config preserves active instance; modified package quarantined; Python config survives restart independently; disabled host stays stopped')
 print('PASS explicit restart restore retains config and version, default-off/disabled stay off, repeated restore does not rerun activation')
