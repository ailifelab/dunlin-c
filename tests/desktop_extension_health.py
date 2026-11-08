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

 ok('compat.enable',node='/opt/homebrew/bin/node')
 plugin=root/'crash';plugin.mkdir()
 manifest={'protocol':'dunlin.extension','major':1,'packageId':'owned-crash','version':'1.0.0','displayName':'Owned crash','kind':'plugin','engines':{'dunlin':'desktop@1','host':'node-dsh','dsh':'0.2.1-rc.3'},'entry':'index.ts','capabilities':['trusted-full-host'],'requires':['dunlin.contributions@1'],'dependencies':[]}
 (plugin/'dunlin.json').write_text(json.dumps(manifest))
 (plugin/'index.ts').write_text("export const inject=['dunlin'];export function apply(ctx){ctx.dunlin.registerCommand(ctx,'schedule',()=>{setTimeout(()=>process.exit(23),200);return {scheduled:true}});ctx.dunlin.contribute(ctx,'page',{kind:'sidebar',title:'Owned page',body:'Removed on exit'})}")
 row=ok('extensions.import',path=str(plugin))[0];ref=dict(source=row['source'],package=row['package'],version=row['version'])
 ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)
 assert ok('extensions.contributions')
 ok('extensions.command',source=ref['source'],package=ref['package'],command='schedule',arguments={})
 import time
 deadline=time.monotonic()+8
 while time.monotonic()<deadline:
  rows=ok('extensions.list')
  if not rows[0]['active']:break
  time.sleep(.05)
 assert not rows[0]['active'] and not rows[0]['enabled'],rows
 assert rows[0]['versions'][0]['quarantined']
 faults=ok('extensions.faults');assert faults and faults[0]['error']=='host_exited',faults
 assert not ok('extensions.contributions')
 count=len(faults)
 for _ in range(3):ok('extensions.list')
 assert len(ok('extensions.faults'))==count,'health checks duplicated fault logs'
 lib.dc_close(db)
 print('PASS asynchronous owned Node host exit detected by inventory, disabled and quarantined, page removed, durable nonduplicate fault')
