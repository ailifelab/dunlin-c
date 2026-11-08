"""Actual pinned upstream packages via C immutable import/host lifecycle.
The previous five rejection expectations are superseded by real effect checks;
historical failure reports remain separate. This does not certify every DSH UI.
"""
import ctypes as C,json,pathlib,sys,tempfile
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P),('dc_add_chat',[P,S,S,S],C.c_int)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
inputs=json.load(open(sys.argv[3]));results=[]
with tempfile.TemporaryDirectory(prefix='dunlin-real-dsh-') as tmp:
 home=pathlib.Path(tmp);workspace=home/'work';workspace.mkdir();state=home/'data';state.mkdir(mode=0o700);db=lib.dc_open(str(state).encode());assert db
 def req(op,**kw):
  ptr=lib.dc_extensions_call(db,json.dumps(dict(op=op,**kw)).encode(),None);assert ptr
  try:return json.loads(C.string_at(ptr))
  finally:lib.dc_free(ptr)
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r['data']
 def http(url,method='GET',body=None):
  value=ok('extensions.command',**ref,chat='chat',command='http',arguments=dict(url=url,method=method,body=body or {}));r=value['data'];assert r['status']<400,r;return r['body']
 try:
  assert lib.dc_add_chat(db,b'chat',b'Owned real plugins',str(workspace).encode());ok('extensions.assets',path=str(pathlib.Path(sys.argv[2]).resolve()));ok('compat.enable',node='/opt/homebrew/bin/node')
  for item in inputs:
   name=item['name'];row=next(x for x in ok('extensions.import',path=str(pathlib.Path(item['path']).resolve())) if x['package']==name);ref={k:row[k] for k in ['source','package','version']};assert not row['active'] and not row['enabled'];config={'landfill':False,'projectSessionMode':'inherit'} if name=='dsh-project-manager' else {}
   ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True,config=config)
   if name=='dsh-better-sidebar':
    http('/sidebar/api/fs.createFile','POST',dict(sessionId='chat',path=str(workspace.resolve()),name='actual.txt'));assert (workspace/'actual.txt').is_file()
   elif name=='dsh-catppuccin':
    http('/catppuccin/state','PUT',{'skin':'catppuccin-mocha'});assert http('/catppuccin/state')['skin']=='catppuccin-mocha';assert len(ok('extensions.themes'))==4
   elif name=='dsh-image-gen':
    value=ok('extensions.command',**ref,chat='chat',command='tool:canvas_state',arguments={});assert not value['data']['isError'],value
   elif name=='dshmarket':assert http('/dsh-market/favorite','POST',{'url':item['repository'] if item['repository'].startswith('https:') else 'https://github.com/'+item['repository'],'favorited':True})['ok']
   elif name=='dsh-project-manager':assert http('/_dsh/dsh-project-manager/group/create','POST',{'name':'Owned group'})['ok']
   elif name=='dsh-checkpoint-rewind':
    value=ok('extensions.command',**ref,chat='chat',command='slash:checkpoint',arguments={'input':'list'});assert value['data']['result']['kind']=='success',value
   ok('extensions.bind',**ref,enabled=False);assert not any(c['plugin']==row['runtimeID'] for c in ok('extensions.contributions'))
   ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True,config=config)
   if name=='dsh-catppuccin':assert http('/catppuccin/state')['skin']=='catppuccin-mocha'
   ok('extensions.uninstall',source=ref['source'],package=ref['package']);assert not any(c['plugin']==row['runtimeID'] for c in ok('extensions.contributions'));assert not req('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)['ok'];results.append(dict(name=name,version=item['version'],status='pass',scope='cached published package, actual host effect, disable/re-enable/uninstall; no external AI provider or full client claim'))
 finally:lib.dc_close(db)
pathlib.Path(sys.argv[4]).write_text(json.dumps(results,indent=2)+'\n')
print('PASS all six unchanged published host entrypoints, actual SDK/route effects and C lifecycle; full Catppuccin UI, npm version update and fault injection verified separately.')
