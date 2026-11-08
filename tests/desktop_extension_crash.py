#!/usr/bin/env python3
"""Kill only an owned C client during Python plugin activation; never replay it."""
import ctypes as C,json,sys,pathlib,tempfile,subprocess,time,sqlite3
libpath=str(pathlib.Path(sys.argv[1]).resolve());assets=str(pathlib.Path(sys.argv[2]).resolve())
lib=C.CDLL(libpath);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P)]:
 f=getattr(lib,n);f.argtypes=a;f.restype=r
class Store:
 def __init__(self,p):self.db=lib.dc_open(str(p).encode());assert self.db
 def close(self):lib.dc_close(self.db)
 def req(self,op,**kw):
  raw=lib.dc_extensions_call(self.db,json.dumps(dict(op=op,**kw)).encode(),None);assert raw
  try:return json.loads(C.string_at(raw))
  finally:lib.dc_free(raw)
 def ok(self,op,**kw):
  r=self.req(op,**kw);assert r['ok'],r;return r.get('data')
if len(sys.argv)>3 and sys.argv[3]=='--child':
 root=pathlib.Path(sys.argv[4]);s=Store(root/'private');s.ok('extensions.assets',path=assets);s.ok('python.configure',executable=sys.executable)
 row=s.ok('extensions.list')[0];s.ok('extensions.bind',**{k:row[k] for k in ['source','package','version']},enabled=True,acceptHostPermissions=True);s.close();raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='dunlin-lifecycle-crash-') as tmp:
 root=pathlib.Path(tmp);private=root/'private';private.mkdir(mode=0o700);plugin=root/'plugin';plugin.mkdir();effect=root/'effect'
 s=Store(private);s.ok('extensions.assets',path=assets);s.ok('python.configure',executable=sys.executable)
 m={'protocol':'dunlin.extension','major':1,'packageId':'crash','version':'1.0.0','displayName':'Owned crash','kind':'plugin','engines':{'dunlin':'desktop@1','host':'python'},'entry':'main.py','capabilities':['trusted-full-host'],'requires':['dunlin.contributions@1'],'dependencies':[]}
 (plugin/'dunlin.json').write_text(json.dumps(m));(plugin/'main.py').write_text("from pathlib import Path\nimport time\ndef activate(api,config):\n p=Path("+repr(str(effect))+");p.write_text(p.read_text()+'x' if p.exists() else 'x')\n time.sleep(2)\n")
 s.ok('extensions.import',path=str(plugin));s.close()
 with open(root/'child.log','w') as log:
  child=subprocess.Popen([sys.executable,'-B',__file__,libpath,assets,'--child',str(root)],stdout=log,stderr=subprocess.STDOUT)
  try:
   end=time.monotonic()+8
   while not effect.exists() and child.poll() is None and time.monotonic()<end:time.sleep(.02)
   assert effect.exists(),(root/'child.log').read_text()
   child.kill();child.wait(timeout=5)
  finally:
   if child.poll() is None:child.kill();child.wait(timeout=5)
 time.sleep(2.2) # Owned extension finishes after its C client's death.
 s=Store(private);s.ok('extensions.assets',path=assets);r=s.ok('extensions.restore');assert r['restored']==0 and effect.read_text()=='x',r
 row=s.ok('extensions.list')[0];assert row['versions'][0]['quarantined'] and not row['enabled']
 assert any(x['phase']=='recovery' for x in s.ok('extensions.faults')),'interrupted activation missing user-visible fault'
 audit=sqlite3.connect(private/'conversations.sqlite');assert audit.execute("SELECT status FROM extension_calls WHERE method='plugins.bind'").fetchall()==[('unknown',)];audit.close();s.close()
print('PASS owned client killed during actual plugin apply; restart quarantines, records recovery fault, keeps unknown lifecycle and never replays effect')
