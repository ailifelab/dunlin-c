#!/usr/bin/env python3
"""Actual Node lifecycle with C-sealed optional client script projection."""
import ctypes as C,json,sys,pathlib,tempfile,sqlite3
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P)]:
 f=getattr(lib,n);f.argtypes=a;f.restype=r
with tempfile.TemporaryDirectory(prefix='dunlin-client-seal-') as tmp:
 root=pathlib.Path(tmp);private=root/'private';private.mkdir(mode=0o700);db=lib.dc_open(str(private).encode());assert db
 def req(op,**kw):
  raw=lib.dc_extensions_call(db,json.dumps(dict(op=op,**kw)).encode(),None);assert raw
  try:return json.loads(C.string_at(raw))
  finally:lib.dc_free(raw)
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r.get('data')
 try:
  assert ok('extensions.clients')==[]
  ok('extensions.assets',path=str(pathlib.Path(sys.argv[2]).resolve()))
  ok('compat.enable',node='/opt/homebrew/bin/node')
  plugin=root/'plugin';plugin.mkdir();code="globalThis.__ModuleLoader__.load({id:'client-fixture',factory:()=>({apply(){}})})"
  manifest={'protocol':'dunlin.extension','major':1,'packageId':'client-fixture','version':'1','kind':'plugin','engines':{'dunlin':'desktop@1','host':'node-dsh','dsh':'0.2.1-rc.3'},'entry':'index.ts','capabilities':['trusted-full-host'],'requires':['dunlin.contributions@1'],'dependencies':[],'client':{'format':'dsh-lazy-cjs@1','entry':'client.js','slots':['conversation.composer.dock']}}
  (plugin/'index.ts').write_text("export const inject=['dunlin'];export function apply(ctx){ctx.dunlin.contribute(ctx,'owned',{kind:'command',title:'Owned client effect',command:'owned'});ctx.dunlin.registerCommand(ctx,'owned',()=>({ok:true}))}");(plugin/'client.js').write_text(code)
  def write(): (plugin/'dunlin.json').write_text(json.dumps(manifest))
  manifest['client']['slots']=['root'];write();assert req('extensions.import',path=str(plugin))['error']=='extension_client_unsupported'
  manifest['client']['slots']=['conversation.composer.dock'];manifest['client']['entry']='../client.js';write();assert req('extensions.import',path=str(plugin))['error']=='extension_client_invalid'
  manifest['client']['entry']='client.js'
  manifest['client']['slots']=['conversation.chat.reasoning.body','conversation.chat.reasoning.body'];write()
  assert req('extensions.import',path=str(plugin))['error']=='extension_client_unsupported'
  manifest['client']['slots']=['shell.overlay','conversation.chat.reasoning.body']
  manifest['client']['entry']='client.js';write();row=ok('extensions.import',path=str(plugin))[0];ref={k:row[k] for k in ('source','package','version')}
  assert ok('extensions.clients')==[]
  ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)
  clients=ok('extensions.clients');assert len(clients)==1 and clients[0]['code']==code and clients[0]['config']=={} and clients[0]['slots']==['shell.overlay','conversation.chat.reasoning.body']
  assert req('extensions.clientFault',**dict(ref,version='stale'))['error']=='stale_client_fault'
  assert len(ok('extensions.clients'))==1
  ok('extensions.clientFault',**ref);assert ok('extensions.clients')==[]
  assert ok('extensions.faults')[0]['phase']=='client'
  assert ok('extensions.contributions')==[], 'quarantined client left host contributions active'
  assert not ok('extensions.list')[0]['active']
  ok('extensions.recover',**ref);ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)
  ok('extensions.bind',**ref,enabled=False);assert ok('extensions.clients')==[]
  ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)
  sql=sqlite3.connect(private/'conversations.sqlite');installed=pathlib.Path(sql.execute('SELECT path FROM extension_versions').fetchone()[0]);sql.close()
  (installed/'client.js').write_text(code+';tampered()')
  assert ok('extensions.clients')==[];assert not ok('extensions.list')[0]['active'];assert ok('extensions.faults');assert ok('extensions.contributions')==[], 'quarantined client left host contributions active'
  ok('compat.disable');assert ok('extensions.clients')==[]
  print('PASS active-only C client projection, disabled host/binding exclusion, slot/path rejection, immutable byte verification and quarantine after tampering; native rendering separately required.')
 finally:lib.dc_close(db)
