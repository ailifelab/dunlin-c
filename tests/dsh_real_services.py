"""Real fixed npm plugins, actual SDK services and owned effects (no model/key).
This is not a replacement for C lifecycle or native rendering acceptance.
"""
import json,os,pathlib,selectors,subprocess,sys,tempfile,time,base64
root=pathlib.Path.cwd();inputs=json.load(open(sys.argv[1]));results=[]
with tempfile.TemporaryDirectory(prefix='dunlin-real-services-') as tmp:
 home=pathlib.Path(tmp);workspace=home/'workspace';workspace.mkdir();workspace=workspace.resolve();(workspace/'seed.txt').write_text('before\n');st=workspace.stat()
 context=dict(chat='owned-chat',path=str(workspace),device=str(st.st_dev),inode=str(st.st_ino),position=dict(turn=1,step=1))
 p=subprocess.Popen(['/opt/homebrew/bin/node',str(root/'hosts/node-dsh/worker.mjs'),str(root/'compat/dsh'),str(root/'build/v51/dsh-toolchain-v2')],env=dict(HOME=tmp,DSH_HOME=str(home/'dsh'),PATH='/usr/bin:/bin',DUNLIN_WORKER_NONCE='b'*64),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
 sel=selectors.DefaultSelector();sel.register(p.stdout,selectors.EVENT_READ);sel.register(p.stderr,selectors.EVENT_READ);seq=0;stderr=[]
 def read():
  end=time.monotonic()+35
  while time.monotonic()<end:
   for key,_ in sel.select(1):
    line=key.fileobj.readline()
    if not line:raise RuntimeError('worker exited: '+''.join(stderr)[-2000:])
    if key.fileobj is p.stdout:return json.loads(line)
    stderr.append(line)
  raise TimeoutError('worker response: '+''.join(stderr)[-2000:])
 def call(method,**params):
  global seq
  seq+=1;p.stdin.write(json.dumps(dict(jsonrpc='2.0',id=seq,method=method,params=params))+'\n');p.stdin.flush();r=read();assert r.get('id')==seq,r
  assert 'result' in r,r
  return r['result']
 def command(name,args):return call('plugins.command',plugin=plugin,command=plugin+':'+name,arguments=args,nativeContext=context)
 def http(url,method='GET',body=None):
  response=command('http',dict(url=url,method=method,body=body or {}));
  if 'bodyHandle' in response:
   chunks=[base64.b64decode(command('http.body',dict(id=response['bodyHandle'],offset=offset))['base64']) for offset in range(0,response['bytes'],98304)];command('http.body',dict(id=response['bodyHandle'],release=True));response['body']=json.loads(b''.join(chunks))
  assert response['status']<400,response;return response
 try:
  read()
  for i,item in enumerate(inputs):
   plugin='real-'+str(i);name=item['name'];config={}
   if name=='dsh-project-manager':config={'landfill':False,'projectSessionMode':'inherit'}
   if name=='dsh-checkpoint-rewind':config={'provider':'copy','snapshotDir':str(home/'snapshots')}
   call('plugins.activate',id=plugin,root=str((root/item['path']).resolve()),entry=item['entry'],config=config)
   effects=[]
   if name=='dsh-better-sidebar':
    response=http('/sidebar/api/fs.createFile','POST',dict(sessionId=context['chat'],path=str(workspace),name='created.txt'))
    assert (workspace/'created.txt').is_file(),response;effects.append('real workspace file created')
    response=http('/sidebar/api/fs.read','POST',dict(sessionId=context['chat'],path=str(workspace/'seed.txt')))
    assert 'before' in json.dumps(response),response;effects.append('real file read')
    channel=command('ws:/sidebar/ws/fs-watch',dict(op='open',url='/sidebar/ws/fs-watch?sessionId='+context['chat']))
    command('ws:/sidebar/ws/fs-watch',dict(op='send',id=channel['id'],text=json.dumps(dict(op='watch',path=str(workspace)))))
    events=command('ws:/sidebar/ws/fs-watch',dict(op='read',id=channel['id']))
    time.sleep(.1);(workspace/'notify.txt').write_text('watch effect')
    received=[];until=time.monotonic()+3
    while time.monotonic()<until:
     received.extend(command('ws:/sidebar/ws/fs-watch',dict(op='read',id=channel['id']))['events'])
     if any(json.loads(e['text'])=={'dir':str(workspace)} for e in received if e.get('type')=='message' and e.get('text')):break
     time.sleep(.1)
    assert any(json.loads(e['text'])=={'dir':str(workspace)} for e in received if e.get('type')=='message' and e.get('text')),received
    command('ws:/sidebar/ws/fs-watch',dict(op='close',id=channel['id']));effects.append('actual websocket handshake and filesystem change notification, close')
   elif name=='dsh-catppuccin':
    http('/catppuccin/state','PUT',{'skin':'catppuccin-mocha','flavor':'mocha'})
    response=http('/catppuccin/state');assert response['body']['skin']=='catppuccin-mocha',response;effects.append('durable theme state')
    themes=[v for v in call('plugins.contributions') if v.get('kind')=='theme'];assert len(themes)==4;effects.append('four native theme projections')
   elif name=='dsh-image-gen':
    png='iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEUlEQVR4nGP4z8DA8B+MgBgAHfAD/dPQfSYAAAAASUVORK5CYII='
    response=http('/plugins/dsh-image-gen/import','POST',{'images':[{'data':png,'mediaType':'image/png','name':'owned.png'}]})
    assert not response['body'].get('failures'),response;assert response['body'].get('images'),response;effects.append('real PNG persisted by upstream attachment store')
    attachment=response['body']['images'][0]['attachment'];response=http('/plugins/dsh-image-gen/storage-stats','POST',{'attachmentIds':[attachment['attachmentId']]});effects.append('actual image storage statistics')
    response=command('tool:canvas_state',{});assert 'error' not in response or not response['error'],response;effects.append('actual SDK canvas tool execution')
   elif name=='dshmarket':
    response=http('/dsh-market/registry');effects.append('upstream registry preferences read')
    response=http('/dsh-market/favorite','POST',{'url':'https://github.com/zhijun-dai/Catppuccin-dsh-theme','favorited':True});effects.append('upstream favorite mutation')
   elif name=='dsh-project-manager':
    response=http('/_dsh/dsh-project-manager/group/create','POST',{'name':'Owned board'})
    group=response['body']['result']['id'];response=http('/_dsh/dsh-project-manager/card/create','POST',{'group':group,'title':'Owned card'})
    card=response['body']['result']['id'];response=http('/_dsh/dsh-project-manager/state');assert any(c['id']==card for c in response['body']['board']['cards']);effects.append('real board/group/card mutation and persisted snapshot')
   elif name=='dsh-checkpoint-rewind':
    response=command('slash:checkpoint',{'input':'note owned acceptance'});assert response['result']['kind']=='success' and 'captured' in response['result']['text'],response;effects.append('actual copy snapshot captured')
    (workspace/'seed.txt').write_text('after\n')
    response=command('slash:checkpoint',{'input':'list'});assert 'owned acceptance' in json.dumps(response),response;effects.append('actual snapshot listed')
    response=command('slash:rewind',{'input':'workspace latest'});assert (workspace/'seed.txt').read_text()=='after\n',response;assert 'denied' in json.dumps(response).lower() or 'approval' in json.dumps(response).lower() or 'fail closed' in json.dumps(response).lower(),response;effects.append('rewind fails closed without approval; file unchanged')
   call('plugins.deactivate',id=plugin);assert not any(c['plugin']==plugin for c in call('plugins.contributions'))
   call('plugins.activate',id=plugin,root=str((root/item['path']).resolve()),entry=item['entry'],config=config)
   if name=='dsh-catppuccin':assert http('/catppuccin/state')['body']['skin']=='catppuccin-mocha'
   if name=='dsh-project-manager':assert any(c['id']==card for c in http('/_dsh/dsh-project-manager/state')['body']['board']['cards'])
   call('plugins.deactivate',id=plugin);results.append(dict(name=name,status='pass',effects=effects));print(name,effects,flush=True)
 finally:
  p.terminate();p.communicate(timeout=10)
pathlib.Path(sys.argv[2]).write_text(json.dumps(results,indent=2)+'\n')
