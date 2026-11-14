"""All maintained exact npm plugins through production C installation/lifecycle.
Owned HOME/workspace only; no model credentials or model command approval.
"""
import ctypes as C,json,pathlib,sys,tempfile,sqlite3,os,signal,subprocess,time
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P),('dc_catalog_call',[P,S,P],P),('dc_add_chat',[P,S,S,S],C.c_int)]:
 f=getattr(lib,n);f.argtypes=a;f.restype=r
pins=json.load(open('catalogs/dsh-test-pins.json'));previous={p['name']:p['previous'] for p in json.load(open(sys.argv[3]))};results=[]
with tempfile.TemporaryDirectory(prefix='dunlin-all-npm-') as tmp:
 home=pathlib.Path(tmp);workspace=(home/'workspace');workspace.mkdir();workspace=workspace.resolve();(workspace/'seed.txt').write_text('owned fixture\n');(home/'data').mkdir(mode=0o700);db=lib.dc_open(str(home/'data').encode());assert db
 def req(op,**kw):
  fn=lib.dc_catalog_call if op.startswith('catalog.') else lib.dc_extensions_call
  raw=fn(db,json.dumps(dict(op=op,**kw)).encode(),None);assert raw
  try:return json.loads(C.string_at(raw))
  finally:lib.dc_free(raw)
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r.get('data')
 def command(name,args,chat=True):
  r=ok('extensions.command',**ref,command=name,arguments=args,**({'chat':'owned-chat'} if chat else {}));assert r['ok'],r;return r['data']
 def http(url,method='GET',body=None):
  r=command('http',dict(url=url,method=method,body=body or {}))
  if 'pendingHandle' in r:
   handle=r['pendingHandle'];until=time.monotonic()+30
   try:
    while True:
     pending=command('http.pending',dict(id=handle))
     if pending.get('ready'):r=pending['response'];break
     assert time.monotonic()<until,pending;time.sleep(.05)
   finally:command('http.pending',dict(id=handle,release=True))
  assert r['status']<400,r
  if 'bodyHandle' in r:
   import base64
   chunks=[base64.b64decode(command('http.body',dict(id=r['bodyHandle'],offset=i))['base64']) for i in range(0,r['bytes'],98304)];command('http.body',dict(id=r['bodyHandle'],release=True));r['body']=json.loads(b''.join(chunks))
  return r['body']
 try:
  assert lib.dc_add_chat(db,b'owned-chat',b'Owned plugin acceptance',str(workspace).encode())
  ok('extensions.assets',path=str(pathlib.Path(sys.argv[2]).resolve()));ok('compat.enable',node='/opt/homebrew/bin/node')
  for pin in pins:
   name=pin['name'];repo='https://github.com/'+pin['repository'];old=previous[name];new=pin['version'];installed=[]
   for version in [old,new]:
    rows=ok('catalog.npm.install',repository=repo,version=version);row=next(r for r in rows if r['package']==name);assert not row['enabled'] and not row['active'],row
    installed.append(version+'+dunlin.3');assert any(v['version']==installed[-1] for v in row['versions']),row
   ref={k:row[k] for k in ['source','package']};assert len(row['versions'])==2,row
   configuration={'projectSessionMode':'inherit','landfill':False} if name=='dsh-project-manager' else {}
   ok('extensions.bind',**ref,version=installed[0],enabled=True,acceptHostPermissions=True,config=configuration)
   ok('extensions.bind',**ref,version=installed[1],enabled=True,acceptHostPermissions=True,config=configuration)
   assert any(r['active'] and r['version']==installed[1] for r in ok('extensions.list')),name
   effects=[]
   if name=='dsh-better-sidebar':
    http('/sidebar/api/fs.createFile','POST',dict(sessionId='owned-chat',path=str(workspace),name='from-sidebar.txt'));assert (workspace/'from-sidebar.txt').is_file();effects.append('C-owned chat -> real file creation')
   elif name=='dsh-catppuccin':
    http('/catppuccin/state','PUT',{'skin':'catppuccin-frappe'});assert http('/catppuccin/state')['skin']=='catppuccin-frappe';assert len(ok('extensions.themes'))==4;assert ok('extensions.clients')[0]['slots']==['settings.general.item'];effects.append('real state route, four themes and full client projection')
    storage=dict(ref,version=installed[1]);ok('extensions.clientStorage.write',**storage,value={'dsh-catppuccin:glow':'off'});assert ok('extensions.clientStorage.read',**storage)=={'dsh-catppuccin:glow':'off'}
    assert not req('extensions.clientStorage.read',**dict(storage,version='stale'))['ok'];assert not req('extensions.clientStorage.write',**storage,value={'oversize':'x'*8193})['ok'];effects.append('owner/version-bound durable client storage')
   elif name=='dsh-image-gen':
    r=command('tool:canvas_state',{});assert not r.get('isError'),r;http('/plugins/dsh-image-gen/settings-health');effects.append('SDK tool and settings health')
   elif name=='dshmarket':
    http('/dsh-market/favorite','POST',{'url':repo,'favorited':True});effects.append('real market favorite persistence')
   elif name=='dsh-project-manager':
    value=http('/_dsh/dsh-project-manager/group/create','POST',{'name':'Owned group'});assert value['result']['name']=='Owned group',value;effects.append('upstream board mutation')
   elif name=='dsh-checkpoint-rewind':
    value=command('slash:checkpoint',{'input':'list'});assert value['result']['kind']=='success',value;effects.append('real slash command registry and session context')
   ok('extensions.bind',**ref,version=installed[1],enabled=False);assert not any(c['plugin']==row['runtimeID'] for c in ok('extensions.contributions'))
   ok('extensions.bind',**ref,version=installed[1],enabled=True,acceptHostPermissions=True,config=configuration)
   if name=='dsh-catppuccin':assert http('/catppuccin/state')['skin']=='catppuccin-frappe';assert ok('extensions.clientStorage.read',**storage)['dsh-catppuccin:glow']=='off'
   # Fault injection: kill only this test process's worker, then require automatic
   # disable and versioned logs. This is a real host loss, not a plugin crash claim.
   processes=subprocess.check_output(['ps','-axo','pid=,ppid=,command='],text=True).splitlines();children=[]
   for line in processes:
    parts=line.strip().split(None,2)
    if len(parts)==3 and int(parts[1])==os.getpid() and '/hosts/node-dsh/worker.mjs' in parts[2]:children.append(int(parts[0]))
   assert len(children)==1,children;os.kill(children[0],signal.SIGKILL)
   until=time.monotonic()+5
   while True:
    rows=ok('extensions.list');current=next(r for r in rows if r['package']==name)
    if not current['active'] and not current['enabled']:break
    assert time.monotonic()<until,current;time.sleep(.05)
   faults=[f for f in ok('extensions.faults') if f['package']==name];assert faults and faults[0]['version']==installed[1],faults
   ok('extensions.uninstall',**ref);assert not any(r['package']==name for r in ok('extensions.list'));assert [f for f in ok('extensions.faults') if f['package']==name]==faults
   assert not req('extensions.command',**ref,command='http',arguments={})['ok']
   with sqlite3.connect(home/'data/conversations.sqlite') as sql:
    for table in ['extension_versions','extension_bindings','extension_configuration','extension_client_storage']:
     assert sql.execute('SELECT count(*) FROM '+table+' WHERE source=? AND package=?',(ref['source'],ref['package'])).fetchone()[0]==0,table
   assert not list((home/'data/extension-store').iterdir()),'unreferenced cache retained'
   results.append(dict(name=name,previous=old,version=new,checks=['install default off','enable old','update and execute new','disable removes contributions','re-enable','real host-loss automatic disable/logs','uninstall all versions/cache/storage','execution denied after uninstall'],effects=effects));print(name,'PASS',flush=True)
   ok('compat.enable',node='/opt/homebrew/bin/node')
 finally:lib.dc_close(db)
pathlib.Path(sys.argv[4]).write_text(json.dumps(results,indent=2)+'\n')
