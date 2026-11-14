"""Owned C/Node integration owner for actual published React workbench tests.
No user HOME/config/credentials; this is a fixture host, not a substitute plugin.
"""
import secrets,sqlite3,time,base64,subprocess,urllib.parse,http.cookies
import ctypes as C,json,pathlib,tempfile,sys,threading,http.server,shutil,os,contextlib
P=C.c_void_p;S=C.c_char_p;root=pathlib.Path.cwd();lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
for n,a,r in [('dc_open',[S],P),('dc_open_agent_worker',[P],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P),('dc_catalog_call',[P,S,P],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_delete_chat',[P,S],C.c_int),('dc_workspace_request',[P,S],P),('dc_agent_execute',[P,S,S,S,P],P),('dc_helper_execute',[P,S,C.c_longlong,S,P],P),('dc_run',[P,S,S,S,P],P),('dc_messages',[P,S],P)]:
 f=getattr(lib,n);f.argtypes=a;f.restype=r
Event=C.CFUNCTYPE(None,P,S,S);Approve=C.CFUNCTYPE(C.c_int,P,S,S);Cancel=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',Event),('approve',Approve),('cancelled',Cancel)]
fixture_consent=False;managed_call=False;fixture_replay_consent=False
@Event
def event(*a):pass
@Approve
def approve(_,name,body):
 # Explicit fixture-only plugin trust policy. This never approves a model tool
 # or any model command. A separately opted-in deterministic session-only
 # replay test can answer its exact original question; never native-user evidence.
 value=json.loads(body)
 if managed_call and os.environ.get('DUNLIN_WORKBENCH_CONFIG_RESTORE') and fixture_replay_consent and name==b'extension.confirm' and value.get('package')=='dsh-checkpoint-rewind' and value.get('version')=='0.6.34+dunlin.3' and value.get('question',{}).get('id')=='rewind-confirm' and value.get('question',{}).get('question')=='Restore the plugin configuration to this checkpoint snapshot?':return 1
 if managed_call and os.environ.get('DUNLIN_WORKBENCH_REPLAY') and fixture_replay_consent and name==b'extension.confirm' and value.get('package')=='dsh-checkpoint-rewind' and value.get('version')=='0.6.34+dunlin.3' and value.get('question',{}).get('id')=='rewind-confirm' and value.get('question',{}).get('question')=='Replay the session context from this checkpoint?':return 1
 return int(managed_call and fixture_consent and name==b'extension.confirm' and value.get('package')=='dsh-checkpoint-rewind' and value.get('version')=='0.6.34+dunlin.3' and value.get('question',{}).get('id')=='managed-install')
@Cancel
def cancelled(*a):return 0
hooks=Hooks(None,event,approve,cancelled)
native_home=pathlib.Path(sys.argv[4]).resolve() if len(sys.argv)>4 else None
if native_home:
 assert native_home.is_relative_to(root/'build') and not native_home.exists();native_home.mkdir(parents=True)
with (contextlib.nullcontext(str(native_home)) if native_home else tempfile.TemporaryDirectory(prefix='dunlin-react-owner-')) as tmp:
 home=pathlib.Path(tmp);os.environ['HOME']=str(home);os.environ['SHELL']='/bin/sh';os.environ.pop('ENV',None);os.environ.pop('BASH_ENV',None);(home/'data').mkdir(mode=0o700);work=home/'workspace';work.mkdir();(work/'seed.txt').write_text('original owned workbench text\n');db=lib.dc_open(str(home/'data').encode());assert db
 if os.environ.get('DUNLIN_TEST_PROGRESS'):print('fixture C store open',file=sys.stderr,flush=True)
 lock=threading.Lock();installed={};calls=[];clipboard_writes=[];token=secrets.token_hex(32)
 def request(op,**args):
  fn=lib.dc_catalog_call if op.startswith('catalog.') else lib.dc_extensions_call
  raw=fn(db,json.dumps(dict(op=op,**args)).encode(),C.byref(hooks) if managed_call else None);assert raw
  try:return json.loads(C.string_at(raw))
  finally:lib.dc_free(raw)
 class RequestError(RuntimeError):
  def __init__(self,response):super().__init__(response);self.response=response
 def core(op,**args):
  pointer=lib.dc_workspace_request(db,json.dumps(dict(op=op,**args)).encode());assert pointer
  try:value=json.loads(C.string_at(pointer))
  finally:lib.dc_free(pointer)
  assert value.get('ok'),value
  return value.get('data')
 def ok(op,**args):
  r=request(op,**args)
  if not r.get('ok'):raise RequestError(r)
  return r['data']
 assert lib.dc_add_chat(db,b'ui-chat',b'Owned React workbench',str(work).encode())
 # A real C model turn supplies canonical history and checkpoint positions.
 model_requests=[];image_requests=[];image_delay=0;text_delay=0;native_threads=[];native_runs=[];fixture_reply='Owned React baseline completed.';text_failure=False;workspace_swaps={}
 class Model(http.server.BaseHTTPRequestHandler):
  def log_message(self,*a):pass
  def do_POST(self):
   payload=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
   if self.path.endswith('/images/generations'):
    image_requests.append({'model':payload.get('model'),'prompt':payload.get('prompt'),'authenticated':self.headers.get('Authorization')=='Bearer fixture-image-key'})
    if not image_requests[-1]['authenticated']:self.send_error(401);return
    time.sleep(image_delay);body=json.dumps({'created':1,'data':[{'b64_json':'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII='}]}).encode()
   else:
    model_requests.append(payload)
    if os.environ.get('DUNLIN_WORKBENCH_LIVE_STREAM') and payload.get('stream'):
     self.send_response(200);self.send_header('Content-Type','text/event-stream');self.end_headers()
     try:
      self.wfile.write(('data: '+json.dumps({'choices':[{'index':0,'delta':{'content':'Owned live prefix · 中文🦆'},'finish_reason':None}]})+'\n\n').encode());self.wfile.flush()
      time.sleep(3)
      self.wfile.write(('data: '+json.dumps({'choices':[{'index':0,'delta':{'content':' settled once.'},'finish_reason':None}]})+'\n\ndata: '+json.dumps({'choices':[{'index':0,'delta':{},'finish_reason':'stop'}]})+'\n\ndata: [DONE]\n\n').encode());self.wfile.flush()
     except (BrokenPipeError,ConnectionResetError):pass
     return
    time.sleep(text_delay)
    if text_failure:self.send_error(503,'Owned fixture unavailable');return
    body=json.dumps({'choices':[{'index':0,'message':{'role':'assistant','content':fixture_reply},'finish_reason':'stop'}]}).encode()
    if payload.get('stream'):body=('data: '+json.dumps({'choices':[{'index':0,'delta':{'content':fixture_reply},'finish_reason':None}]})+'\n\ndata: '+json.dumps({'choices':[{'index':0,'delta':{},'finish_reason':'stop'}]})+'\n\ndata: [DONE]\n\n').encode()
   self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers()
   try:self.wfile.write(body)
   except BrokenPipeError:pass
 model=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=model.serve_forever,daemon=True).start()
 try:
  cfg=json.dumps(dict(base_url=f'http://127.0.0.1:{model.server_port}/v1',model='owned',protocol='chat',stream=False,desktop_tools='noTools')).encode();pointer=lib.dc_run(db,b'ui-chat',cfg,b'Owned baseline for React checkpoint and fork',None);assert pointer
  try:assert json.loads(C.string_at(pointer))['status']=='completed'
  finally:lib.dc_free(pointer)
 finally:pass
 # Explicit legacy-input fixture: a historical source did not contain event
 # timestamps. Never used in native user acceptance or normal model tests.
 if os.environ.get('DUNLIN_WORKBENCH_HISTORY_FIXTURE'):
  with sqlite3.connect(home/'data/conversations.sqlite') as sql:
   sql.create_function('dunlin_workspace_writer_v1',0,lambda:1)
   row=sql.execute("SELECT state FROM chats WHERE id='ui-chat'").fetchone();legacy=json.loads(row[0])
   for message in legacy['messages']:message.pop('_dunlin_time',None)
   sql.execute("UPDATE chats SET state=? WHERE id='ui-chat'",(json.dumps(legacy,ensure_ascii=False,separators=(',',':')),))
 if os.environ.get('DUNLIN_WORKBENCH_NATIVE_AGENTS'):
  fixture_model=dict(id='owned-a',name='Owned native A',endpoint=f'http://127.0.0.1:{model.server_port}/v1',model='owned-a',protocolName='chat',maxOutput=256,authentication='none')
  if os.environ.get('DUNLIN_WORKBENCH_MODEL_URL'):
   endpoint=os.environ['DUNLIN_WORKBENCH_MODEL_URL'];parsed=urllib.parse.urlparse(endpoint);assert parsed.scheme=='http' and parsed.hostname=='127.0.0.1' and parsed.port and parsed.path=='/v1' and not parsed.username and not parsed.password;fixture_model['endpoint']=endpoint
  core('config.save',revision=0,models=[fixture_model,dict(fixture_model,id='owned-b',name='Owned native B',model='owned-b')],providers=[]);core('chats.update',id='ui-chat',field='model',value='owned-a')
 ok('extensions.assets',path=str(pathlib.Path(sys.argv[2]).resolve()));ok('compat.enable',node='/opt/homebrew/bin/node')
 if os.environ.get('DUNLIN_TEST_PROGRESS'):print('fixture Node ready',file=sys.stderr,flush=True)
 inputs=json.load(open(sys.argv[3]))
 try:
  for item in inputs:
   name=item['name']
   if name=='dsh-catppuccin' and not os.environ.get('DUNLIN_WORKBENCH_THEME'):continue
   source=root/item['path'];target=home/'packages'/name;shutil.copytree(source,target)
   manifest=json.loads((target/'dunlin.json').read_text());manifest['version']=item['version']+'+dunlin.3';manifest['adapterRevision']=3;manifest['upstreamVersion']=item['version'];manifest['client']={'format':'dsh-lazy-cjs@1','entry':'client/client.js' if name=='dshmarket' else 'client/index.js' if name=='dsh-checkpoint-rewind' else 'lib/client.js','slots':['settings.general.item'] if name=='dsh-catppuccin' else ['dunlin.workbench']};(target/'dunlin.json').write_text(json.dumps(manifest))
   rows=ok('extensions.import',path=str(target));row=next(r for r in rows if r['package']==name);ref={k:row[k] for k in ['source','package','version']};config={'landfill':bool(os.environ.get('DUNLIN_WORKBENCH_PROJECT_SEND')),'projectSessionMode':'inherit'} if name=='dsh-project-manager' else {'provider':'copy','snapshotDir':str(home/'snapshots')} if name=='dsh-checkpoint-rewind' else {}
   ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True,config=config);installed[name]=ref
   if os.environ.get('DUNLIN_TEST_PROGRESS'):print('fixture active '+name,file=sys.stderr,flush=True)
  if os.environ.get('DUNLIN_WORKBENCH_PROJECTION'):
   target=home/'projection-probe';target.mkdir();shutil.copyfile(root/'tests/fixtures/native_session_probe.ts',target/'index.ts')
   manifest={'protocol':'dunlin.extension','major':1,'packageId':'native-session-probe','version':'1.0.0','kind':'plugin','entry':'index.ts','engines':{'dunlin':'desktop@1','host':'node-dsh','dsh':'0.2.1-rc.3'},'capabilities':['trusted-full-host'],'requires':['dunlin.contributions@1'],'dependencies':[]}
   (target/'dunlin.json').write_text(json.dumps(manifest));rows=ok('extensions.import',path=str(target));row=next(row for row in rows if row['package']=='native-session-probe');ref={k:row[k] for k in ['source','package','version']};ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True,config={});installed['native-session-probe']=ref
  if os.environ.get('DUNLIN_WORKBENCH_ONLY')=='dsh-panda-calendar':
   name='dsh-panda-calendar';rows=ok('catalog.npm.install',package=name,repository='https://github.com/runcat-tommy/dsh-panda-calendar',version='1.2.5')
   item=next(row for row in rows if row['package']==name);assert not item['enabled'];ref={k:item[k] for k in ['source','package','version']}
   ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True);installed[name]=ref
  pet_packages={
   'dsh-pet':('PC2005-cloud/dsh-pet','0.1.2'),
   'dsh-pet-sayaka':('KOGGUA-EDIT/dsh-pet-sayaka','1.1.1'),
   'dsh-pet-dafeiyu':('SPECIALLOVE96/Fairy','0.1.0'),
  }
  pet=os.environ.get('DUNLIN_WORKBENCH_ONLY')
  selected_pets=list(pet_packages) if pet=='pet-gallery' else [pet] if pet in pet_packages else []
  if pet=='pet-next':
   pet_packages.update({'dsh-pet':('PC2005-cloud/dsh-pet','0.4.8'),'dsh-pet-live2d':('A8Chann/dsh-pet-live2d','3.6.0')})
   selected_pets=['dsh-pet-live2d','dsh-pet']
  for pet in selected_pets:
   repository,version=pet_packages[pet]
   rows=ok('catalog.npm.install',package=pet,repository='https://github.com/'+repository,version=version)
   row=next(row for row in rows if row['package']==pet);assert not row['enabled']
   ref={k:row[k] for k in ['source','package','version']}
   ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True);installed[pet]=ref
  clients=ok('extensions.clients')
  if native_home:
   if os.environ.get('DUNLIN_WORKBENCH_QUIESCE'):ok('extensions.quiesce')
   if os.environ.get('DUNLIN_WORKBENCH_NATIVE_SERVICE'):
    # This process now owns ONLY the loopback model fixture, never the app DB.
    lib.dc_close(db);db=None
    print(json.dumps({'data':str(home/'data'),'workspace':str(work),'packages':list(installed),'modelFixture':f'http://127.0.0.1:{model.server_port}/v1','scope':'owned text-only model fixture; no external provider/model approval acceptance'}),flush=True);sys.stdin.readline()
   else:print(json.dumps({'data':str(home/'data'),'workspace':str(work),'packages':list(installed)}),flush=True)
   raise SystemExit(0)
  def invoke(name,command,args):
   if os.environ.get('DUNLIN_TRACE_CALLS'):print('fixture invoke '+name+':'+command,file=sys.stderr,flush=True)
   ref=installed[name];calls.append({'plugin':name,'command':command})
   if command=='native.asset.read':
    prefix='/dunlin-assets/tldraw/5.3.0/';path=args.get('path','');assert path.startswith(prefix) and '..' not in pathlib.PurePosixPath(path).parts
    base=pathlib.Path(sys.argv[2]).resolve();relative='web/'+path[len('/dunlin-assets/'):];manifest=json.loads((base/'assets.json').read_text());expected=next(item['sha256'] for item in manifest['files'] if item['path']==relative);data=(base/relative).read_bytes();assert len(data)<=16777216
    import hashlib,mimetypes
    assert hashlib.sha256(data).hexdigest()==expected;return {'base64':base64.b64encode(data).decode(),'type':mimetypes.guess_type(path)[0] or 'application/octet-stream'}
   if command=='native.snapshot':return ok('extensions.clientContext',**ref,chat='ui-chat')
   if command=='config.set':
    saved=ok('extensions.config.get',**ref);config=saved['config'];config[args['field']]=args['value'];ok('extensions.settings.apply',**ref,chat='ui-chat',expectedHash=saved['hash'],config=config);return config
   if command.startswith('native.'):
    method=command[7:]
    if method=='clipboard.write':
     ok('extensions.clientContext',**ref,chat='ui-chat');text=args.get('text');assert isinstance(text,str) and '\0' not in text and len(text.encode())<=65536;clipboard_writes.append(text);return {'copied':True}
    if method=='session.open':
     snapshot=ok('extensions.clientContext',**ref,chat='ui-chat');assert args['id'] in snapshot['sessions']['byId'];return {'snapshot':snapshot,'value':True}
    return ok('extensions.clientAction',**ref,chat='ui-chat',method=method,arguments=args)
   global managed_call
   managed_call=(name=='dshmarket' and command=='http' and args.get('url') in ['/dsh-market/install','/dsh-market/update','/dsh-market/toggle']) or (bool(os.environ.get('DUNLIN_WORKBENCH_REPLAY')) and name=='dsh-checkpoint-rewind' and command=='slash:rewind' and __import__('re').fullmatch(r'(?:session|config) [A-Za-z0-9_-]{8,128}',args.get('input','')) is not None)
   try:result=ok('extensions.command',**ref,chat='ui-chat',command=command,arguments=args)
   finally:managed_call=False
   if os.environ.get('DUNLIN_TRACE_CALLS'):print('fixture command returned '+command,file=sys.stderr,flush=True)
   if not result.get('ok'):raise RequestError(result)
   return result['data']
  class Handler(http.server.BaseHTTPRequestHandler):
   def log_message(self,*args):pass
   def do_GET(self):
    if not os.environ.get('DUNLIN_WORKBENCH_BROWSER'):self.send_error(404);return
    url=urllib.parse.urlsplit(self.path);cookie=http.cookies.SimpleCookie();cookie.load(self.headers.get('Cookie',''));ticket=cookie.get('DunlinFixture');authorized=(ticket is not None and secrets.compare_digest(ticket.value,token)) or secrets.compare_digest(urllib.parse.parse_qs(url.query).get('token',[''])[0],token)
    if not authorized:self.send_error(403);return
    headers={};status=200
    try:
     if url.path.startswith('/workbench/'):
      name=url.path[len('/workbench/'):];client=next(c for c in clients if c['package']==name);template=(root/'tests/dsh_workbench_browser.html').read_text();data=template.replace('__FIXTURE_INIT__',json.dumps({'token':token,'client':{'package':name,'config':client['config']}}).replace('<','\\u003c')).encode();content_type='text/html; charset=utf-8';headers['Set-Cookie']='DunlinFixture='+token+'; Path=/; HttpOnly; SameSite=Strict'
     elif url.path in ['/test-assets/runtime.js','/test-assets/runtime.css']:
      name=url.path.rsplit('/',1)[-1];data=(pathlib.Path(sys.argv[2])/'web'/name).read_bytes();content_type='text/javascript' if name.endswith('.js') else 'text/css'
     elif url.path.startswith('/dunlin-assets/tldraw/5.3.0/'):
      with lock:asset=invoke('dsh-image-gen','native.asset.read',{'path':url.path})
      data=base64.b64decode(asset['base64']);content_type=asset['type']
     elif url.path.startswith('/test-client/'):
      name=url.path[len('/test-client/'):-3];client=next(c for c in clients if c['package']==name);data=client['code'].encode();content_type='text/javascript'
     else:
      name='dsh-better-sidebar' if url.path.startswith('/sidebar/') else 'dsh-image-gen' if url.path.startswith('/plugins/dsh-image-gen/') else 'dsh-project-manager' if url.path.startswith('/_dsh/dsh-project-manager/') else 'dshmarket' if url.path.startswith('/dsh-market/') else None
      if not name:self.send_error(404);return
      with lock:response=invoke(name,'http',{'url':self.path,'method':'GET','_surface':'fixture-resource'})
      if response.get('pendingHandle'):
       handle=response['pendingHandle']
       try:
        for poll in range(1200):
         with lock:pending=invoke(name,'http.pending',{'id':handle,'_surface':'fixture-resource'})
         if pending.get('ready'):response=pending['response'];break
         time.sleep(.05)
        else:raise RuntimeError('Fixture resource timeout')
       finally:
        with lock:invoke(name,'http.pending',{'id':handle,'release':True,'_surface':'fixture-resource'})
      if response.get('bodyHandle'):
       handle=response['bodyHandle'];parts=[]
       try:
        for offset in range(0,response['bytes'],98304):
         with lock:part=invoke(name,'http.body',{'id':handle,'offset':offset,'_surface':'fixture-resource'})
         parts.append(base64.b64decode(part['base64']))
        data=b''.join(parts)
       finally:
        with lock:invoke(name,'http.body',{'id':handle,'release':True,'_surface':'fixture-resource'})
      else:data=base64.b64decode(response['bodyBase64']) if 'bodyBase64' in response else json.dumps(response.get('body')).encode()
      status=response['status'];headers=response.get('headers',{});content_type=headers.pop('content-type','application/octet-stream')
     self.send_response(status);self.send_header('Content-Type',content_type);self.send_header('Content-Length',str(len(data)));self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff')
     for key,value in headers.items():
      if key.lower() in ['content-security-policy','set-cookie']:self.send_header(key,value)
     self.end_headers();self.wfile.write(data)
    except (BrokenPipeError,ConnectionResetError):pass
    except Exception as error:self.send_error(500,str(error))
   def do_POST(self):
    origin=self.headers.get('Origin');browser_origin='http://127.0.0.1:'+str(self.server.server_port)
    if self.headers.get('X-Dunlin-Owner')!=token or (origin is not None and (not os.environ.get('DUNLIN_WORKBENCH_BROWSER') or origin!=browser_origin)):self.send_error(403);return
    n=int(self.headers.get('content-length','0'))
    if n>1048576:self.send_error(413);return
    q=json.loads(self.rfile.read(n))
    try:
     with lock:
      if self.path=='/invoke':r={'ok':True,'value':invoke(q['plugin'],q['command'],q.get('args',{}))}
      elif self.path=='/browser-fault':
       assert os.environ.get('DUNLIN_WORKBENCH_BROWSER');r={'ok':True,'value':ok('extensions.clientFault',**installed[q['plugin']])}
      elif self.path=='/restart':
       global db
       lib.dc_close(db);db=lib.dc_open(str(home/'data').encode());assert db;r={'ok':True,'value':True}
      elif self.path=='/delete-fixture-chat':r={'ok':True,'value':bool(lib.dc_delete_chat(db,b'ui-chat'))}
      elif self.path=='/native-text-fixture':
       assert os.environ.get('DUNLIN_WORKBENCH_NATIVE_AGENTS')
       global text_delay,fixture_reply,text_failure
       text_delay=float(q.get('delay',0));assert 0<=text_delay<=25
       text_failure=q.get('fail') is True
       fixture_reply=' ' if q.get('empty') else ('语🦆Ω'+('汉文🦆Ω'*20000)+'末尾') if q.get('large') else 'Owned React baseline completed.';r={'ok':True,'value':{'bytes':len(fixture_reply.encode())}}
      elif self.path=='/native-agents-start':
       assert os.environ.get('DUNLIN_WORKBENCH_NATIVE_AGENTS');jobs=core('agents.nativePending');started=[]
       for job in jobs:
        assert job['id'].startswith('sdk-') and job['model'] in ['owned-a','owned-b'];settings=core('config.model',id=job['model'],revision=core('config.list')['revision']);settings['stream']=bool(os.environ.get('DUNLIN_WORKBENCH_LIVE_STREAM'));worker=lib.dc_open_agent_worker(db);assert worker
        def execute_task(worker=worker,job=job,settings=settings):
         try:
          pointer=lib.dc_agent_execute(worker,job['id'].encode(),json.dumps(settings).encode(),b'',None);assert pointer
          try:native_runs.append({'id':job['id'],'chat':job['chat'],'run':json.loads(C.string_at(pointer))})
          finally:lib.dc_free(pointer)
         finally:lib.dc_close(worker)
        thread=threading.Thread(target=execute_task);native_threads.append(thread);thread.start();started.append(job['id'])
       r={'ok':True,'value':{'started':started}}
      elif self.path=='/native-helpers-start':
       assert os.environ.get('DUNLIN_WORKBENCH_NATIVE_AGENTS');jobs=core('helpers.pending');started=[]
       for job in jobs:
        assert job['model'] in ['owned-a','owned-b'];worker=lib.dc_open_agent_worker(db);assert worker
        def execute_helper(worker=worker,job=job):
         try:
          pointer=lib.dc_helper_execute(worker,job['id'].encode(),job['revision'],b'',None);assert pointer
          try:native_runs.append({'id':job['id'],'helper':True,'run':json.loads(C.string_at(pointer))})
          finally:lib.dc_free(pointer)
         finally:lib.dc_close(worker)
        thread=threading.Thread(target=execute_helper);native_threads.append(thread);thread.start();started.append(job['id'])
       r={'ok':True,'value':{'started':started}}
      elif self.path=='/native-helper-audit':
       assert os.environ.get('DUNLIN_WORKBENCH_NATIVE_AGENTS')
       triggers={
        'plan':"BEFORE INSERT ON model_helper_plans",
        'submit':"BEFORE INSERT ON extension_native_effects WHEN NEW.method='helper.submit'",
        'effect-receipt':"BEFORE UPDATE ON extension_native_effects WHEN NEW.method='helper.submit' AND NEW.status='reported'",
        'handoff':"BEFORE UPDATE ON model_helper_plans WHEN NEW.state='handed_off'",
        'result':"BEFORE UPDATE ON model_helper_jobs WHEN NEW.state='reported'",
        'cleanup':"BEFORE DELETE ON model_helper_jobs",
        'engine-stop':"BEFORE UPDATE ON extension_calls WHEN NEW.status='cancelled_effects_possible' AND EXISTS(SELECT 1 FROM model_helper_plans p WHERE p.call=NEW.id)",
       }
       with sqlite3.connect(home/'data/conversations.sqlite') as sql:
        sql.execute('DROP TRIGGER IF EXISTS deny_helper_owned')
        if q.get('deny'):sql.execute("CREATE TRIGGER deny_helper_owned "+triggers[q['deny']]+" BEGIN SELECT RAISE(ABORT,'owned helper audit failure');END")
       r={'ok':True,'value':True}
      elif self.path=='/native-helper-profile-change':
       assert os.environ.get('DUNLIN_WORKBENCH_NATIVE_AGENTS');registry=core('config.list');registry['models'][0]['name']='Owned changed profile'
       if q.get('smallContext'):registry['models'][0].update(contextLimit=2048,contextBudget=2048)
       r={'ok':True,'value':core('config.save',revision=registry['revision'],models=registry['models'],providers=registry['providers'])}
      elif self.path=='/native-helpers-state':
       assert os.environ.get('DUNLIN_WORKBENCH_NATIVE_AGENTS')
       with sqlite3.connect((home/'data/conversations.sqlite').resolve().as_uri()+'?mode=ro',uri=True) as sql:
        jobs=[dict(zip(['id','call','attempt','tokens','state','text','error'],row)) for row in sql.execute('SELECT id,call,attempt,tokens,state,text,error FROM model_helper_jobs ORDER BY rowid')]
        plans=[dict(zip(['call','model','revision','state'],row)) for row in sql.execute('SELECT call,model,revision,state FROM model_helper_plans ORDER BY rowid')]
        effects=[dict(zip(['call','method','status'],row)) for row in sql.execute("SELECT call,method,status FROM extension_native_effects WHERE method LIKE 'helper.%' ORDER BY rowid")]
        parent_row=sql.execute("SELECT state FROM chats WHERE id='ui-chat'").fetchone();parent=parent_row[0] if parent_row else None
        counts={'chats':sql.execute('SELECT count(*) FROM chats').fetchone()[0],'projects':sql.execute('SELECT count(*) FROM projects').fetchone()[0]}
       r={'ok':True,'value':{'jobs':jobs,'plans':plans,'effects':effects,'parent':parent,'counts':counts,'requests':model_requests,'runs':native_runs,'inventory':ok('extensions.list'),'faults':ok('extensions.faults')}}
      elif self.path=='/native-agents-results':r={'ok':True,'value':{'running':sum(t.is_alive() for t in native_threads),'runs':native_runs,'requests':len(model_requests)}}
      elif self.path=='/native-agents-run':
       assert os.environ.get('DUNLIN_WORKBENCH_NATIVE_AGENTS');jobs=core('agents.pending');runs=[]
       for job in jobs:
        assert job['id'].startswith('sdk-') and job['model'] in ['owned-a','owned-b'];revision=core('config.list')['revision'];settings=core('config.model',id=job['model'],revision=revision);settings['stream']=False
        worker=lib.dc_open_agent_worker(db);assert worker
        try:
         pointer=lib.dc_agent_execute(worker,job['id'].encode(),json.dumps(settings).encode(),b'',None);assert pointer
         try:runs.append({'id':job['id'],'chat':job['chat'],'run':json.loads(C.string_at(pointer))})
         finally:lib.dc_free(pointer)
        finally:lib.dc_close(worker)
       r={'ok':True,'value':{'runs':runs,'jobs':core('agents.list'),'requests':model_requests}}
      elif self.path=='/native-fixture-model-clear':
       assert os.environ.get('DUNLIN_WORKBENCH_NATIVE_AGENTS') and q['chat']!='ui-chat';r={'ok':True,'value':core('chats.update',id=q['chat'],field='model',value='')}
      elif self.path=='/native-state-fixture':
       assert os.environ.get('DUNLIN_WORKBENCH_NATIVE_AGENTS') and isinstance(q.get('chat'),str)
       with sqlite3.connect((home/'data/conversations.sqlite').resolve().as_uri()+'?mode=ro',uri=True) as sql:
        row=sql.execute('SELECT state FROM chats WHERE id=?',(q['chat'],)).fetchone();assert row and row[0];r={'ok':True,'value':json.loads(row[0])}
      elif self.path=='/native-workspace-swap':
       assert os.environ.get('DUNLIN_WORKBENCH_HISTORY_FIXTURE') and q['chat']!='ui-chat' and not any(t.is_alive() for t in native_threads)
       cid=q['chat'];view=core('chats.workspace.get',chat=cid);path=pathlib.Path(view['path']);assert path.resolve().is_relative_to(home.resolve()) and path!=home and path.is_dir()
       if q.get('restore'):
        saved=workspace_swaps.pop(cid);path.rmdir();saved.rename(path)
       else:
        assert view['available'] and cid not in workspace_swaps;saved=path.with_name(path.name+'.owned-before-inode');assert not saved.exists();path.rename(saved);path.mkdir();workspace_swaps[cid]=saved
       r={'ok':True,'value':core('chats.workspace.get',chat=cid)}
      elif self.path=='/native-legacy-event-time':
       # Construct a named legacy test input only. Production never backfills
       # or rewrites historical times; this private fixture tests mixed ages.
       assert os.environ.get('DUNLIN_WORKBENCH_HISTORY_FIXTURE') and q['chat']!='ui-chat' and not any(t.is_alive() for t in native_threads)
       view=core('chats.workspace.get',chat=q['chat']);assert pathlib.Path(view['path']).resolve().is_relative_to(home.resolve())
       with sqlite3.connect(home/'data/conversations.sqlite') as sql:
        sql.create_function('dunlin_workspace_writer_v1',0,lambda:1);row=sql.execute('SELECT state FROM chats WHERE id=?',(q['chat'],)).fetchone();legacy=json.loads(row[0])
        for message in legacy['messages']:message.pop('_dunlin_time',None)
        sql.execute('UPDATE chats SET state=? WHERE id=?',(json.dumps(legacy,ensure_ascii=False,separators=(',',':')),q['chat']))
       r={'ok':True,'value':{'legacyMessages':len(legacy['messages'])}}
      elif self.path=='/native-agent-jobs':r={'ok':True,'value':{'pending':core('agents.nativePending'),'jobs':core('agents.list'),'modelRequests':len(model_requests)}}
      elif self.path=='/native-runtime-disable':assert os.environ.get('DUNLIN_WORKBENCH_NATIVE_AGENTS');r={'ok':True,'value':ok('compat.disable')}
      elif self.path=='/native-runtime-restart':
       assert os.environ.get('DUNLIN_WORKBENCH_NATIVE_AGENTS');ok('compat.disable');ok('compat.enable',node='/opt/homebrew/bin/node');restored=ok('extensions.restore');assert restored['failed']==0,restored;r={'ok':True,'value':restored}
      elif self.path=='/native-agent-audit':
       with sqlite3.connect(home/'data/conversations.sqlite') as sql:
        if q.get('deny')=='client-effect':sql.execute("CREATE TRIGGER deny_client_effect BEFORE INSERT ON extension_calls WHEN NEW.method='jobs.kill' BEGIN SELECT RAISE(ABORT,'owned client effect audit failure');END")
        elif q.get('deny')=='followup':sql.execute("CREATE TRIGGER deny_native_followup BEFORE INSERT ON extension_native_effects WHEN NEW.method='agent.followup' BEGIN SELECT RAISE(ABORT,'owned native admission failure');END")
        elif q.get('deny')=='queue':sql.execute("CREATE TRIGGER deny_native_queue BEFORE INSERT ON agent_jobs WHEN NEW.id LIKE 'sdk-%' BEGIN SELECT RAISE(ABORT,'owned queue failure');END")
        elif q.get('deny')=='receipt':sql.execute("CREATE TRIGGER deny_native_receipt BEFORE UPDATE ON extension_calls WHEN NEW.status='reported' AND EXISTS(SELECT 1 FROM extension_agent_jobs a WHERE a.call=NEW.id AND a.state IN('admitted','ready')) BEGIN SELECT RAISE(ABORT,'owned terminal receipt failure');END")
        elif q.get('deny') in ['config-intent','config-receipt','config-ack-receipt','config-ack-write']:
         if q['deny']=='config-ack-write':sql.execute("CREATE TRIGGER deny_config_receipt BEFORE UPDATE ON extension_native_settings WHEN NEW.status='applied' BEGIN SELECT RAISE(ABORT,'owned config ack storage fault');END")
         else:
          insert=q['deny']=='config-intent';method='settings.applied' if q['deny']=='config-ack-receipt' else 'settings.commit';sql.execute("CREATE TRIGGER deny_config_receipt BEFORE "+('INSERT' if insert else 'UPDATE')+" ON extension_native_effects WHEN NEW.method='"+method+"'"+('' if insert else " AND NEW.status='reported'")+" BEGIN SELECT RAISE(ABORT,'owned config receipt fault');END")
        elif q.get('deny') in ['replay-receipt','notice-receipt']:
         method='session.replay' if q['deny']=='replay-receipt' else 'session.notice';sql.execute("CREATE TRIGGER deny_replay_receipt BEFORE UPDATE ON extension_native_effects WHEN NEW.method='"+method+"' AND NEW.status='reported' BEGIN SELECT RAISE(ABORT,'owned replay receipt failure');END")
        elif q.get('deny')=='effect-receipt':sql.execute("CREATE TRIGGER deny_native_effect_receipt BEFORE UPDATE ON extension_native_effects WHEN NEW.method='agent.followup' AND NEW.status='reported' BEGIN SELECT RAISE(ABORT,'owned subaction receipt failure');END")
        else:
         for trigger in ['deny_client_effect','deny_native_followup','deny_native_queue','deny_native_receipt','deny_native_effect_receipt','deny_replay_receipt','deny_config_receipt']:sql.execute('DROP TRIGGER IF EXISTS '+trigger)
       r={'ok':True,'value':True}
      elif self.path=='/native-audit-state':
       assert os.environ.get('DUNLIN_WORKBENCH_REPLAY')
       with sqlite3.connect((home/'data/conversations.sqlite').resolve().as_uri()+'?mode=ro',uri=True) as sql:
        effects=[dict(zip(['call','method','status'],row)) for row in sql.execute('SELECT call,method,status FROM extension_native_effects ORDER BY rowid')];outer=[dict(zip(['id','package','method','status'],row)) for row in sql.execute('SELECT id,package,method,status FROM extension_calls ORDER BY rowid')];children=[row[0] for row in sql.execute('SELECT chat FROM extension_agent_sessions ORDER BY rowid')]
       r={'ok':True,'value':{'effects':effects,'outer':outer,'children':children,'inventory':ok('extensions.list'),'faults':ok('extensions.faults'),'modelRequests':len(model_requests),'config':ok('extensions.config.get',**installed['dsh-checkpoint-rewind'])}}
      elif self.path=='/probe-native':
       assert 'native-session-probe' in installed and (q['chat'] in ['ui-chat','foreign-chat'] or bool(os.environ.get('DUNLIN_WORKBENCH_REPLAY')) and q['chat'] in request('extensions.clientContext',**installed['native-session-probe'],chat='ui-chat').get('data',{}).get('sessions',{}).get('byId',{})) and q.get('target') in [None,'ui-chat','foreign-chat']
       if q['chat']=='foreign-chat':
        assert lib.dc_add_chat(db,b'foreign-chat',b'Owned foreign projection',str(work).encode()) or request('extensions.clientContext',**installed['native-session-probe'],chat='foreign-chat')['ok']
       assert q.get('mode') is None or (os.environ.get('DUNLIN_WORKBENCH_CWD_PROBE') and q['mode']=='cwd') or (os.environ.get('DUNLIN_WORKBENCH_SOURCE_TIME') and q['mode']=='source-time-bounds')
       result=ok('extensions.command',**installed['native-session-probe'],chat=q['chat'],command='probe',arguments={'target':q.get('target')} if q.get('target') else {'chat':q['chat'],**({'mode':q['mode']} if q.get('mode') else {})});assert result['ok'],result;r={'ok':True,'value':result['data']}
      elif self.path=='/worker-parallel':
       from concurrent.futures import ThreadPoolExecutor
       chats=q['chats'];assert len(chats)==2 and len(set(chats))==2
       snapshot=ok('extensions.clientContext',**next(iter(installed.values())),chat='ui-chat');assert all(cid in snapshot['sessions']['byId'] for cid in chats)
       identities={row['package']:row['instance'] for row in ok('extensions.clients')};handles=[lib.dc_open_agent_worker(db) for _ in chats];assert all(handles)
       def wrequest(handle,op,**args):
        raw=lib.dc_extensions_call(handle,json.dumps(dict(op=op,**args)).encode(),None);assert raw
        try:value=json.loads(C.string_at(raw))
        finally:lib.dc_free(raw)
        assert value.get('ok'),value
        return value['data']
       barrier=threading.Barrier(3);results=[]
       def model_worker(i):
        handle=handles[i];barrier.wait()
        for _ in range(5):
         rows=wrequest(handle,'extensions.clients');assert {row['package']:row['instance'] for row in rows}==identities,{'worker':i,'clients':len(rows),'expected':len(identities)}
         assert wrequest(handle,'compat.status')['healthy']
        settings=json.loads(cfg);settings['desktop_tools']='ask';text='Owned shared worker '+str(i);raw=lib.dc_run(handle,chats[i].encode(),json.dumps(settings).encode(),text.encode(),None);assert raw
        try:run=json.loads(C.string_at(raw))
        finally:lib.dc_free(raw)
        assert run['status']=='completed',run
        return run
       try:
        with ThreadPoolExecutor(max_workers=2) as pool:
         futures=[pool.submit(model_worker,i) for i in range(2)];barrier.wait()
         for _ in range(8):assert invoke('dsh-project-manager','native.snapshot',{})['sessions']['byId'][chats[0]]
         results=[future.result() for future in futures]
        lib.dc_close(handles[0]);handles[0]=None;assert len(ok('extensions.clients'))==len(identities)
        # A fault is shared; closing the UI store must not stop the other worker.
        ok('extensions.clientFault',**installed['dsh-better-sidebar'],instance=identities['dsh-better-sidebar']);assert len(wrequest(handles[1],'extensions.clients'))==len(identities)-1
        owner=db;db=handles[1];handles[1]=None;lib.dc_close(owner);assert ok('compat.status')['healthy'];assert len(ok('extensions.clients'))==len(identities)-1
        requests=[payload for payload in model_requests if any('Owned shared worker ' in str(message.get('content')) for message in payload.get('messages',[]))]
        assert len(requests)==2;r={'ok':True,'value':{'runs':results,'requests':requests,'sameInstances':True,'faultShared':True,'workerCloseRetainsHost':True,'ownerCloseRetainsWorker':True}}
       finally:
        for handle in handles:
         if handle:lib.dc_close(handle)
      elif self.path=='/terminal-child-status':
       child=int((work/'terminal-child.txt').read_text());leader=int((work/'terminal-leader.txt').read_text());assert child>1 and leader>1
       result=subprocess.run(['ps','-o','stat=','-p',str(child)],capture_output=True,text=True);status=result.stdout.strip();r={'ok':True,'value':{'running':bool(status and not status.startswith('Z'))}}
      elif self.path=='/terminal-audit':
       with sqlite3.connect(home/'data/conversations.sqlite') as sql:
        if q.get('deny'):sql.execute("CREATE TRIGGER deny_terminal_intent BEFORE INSERT ON extension_calls WHEN NEW.method LIKE 'terminal.%' BEGIN SELECT RAISE(ABORT,'owned terminal audit failure');END")
        else:sql.execute('DROP TRIGGER deny_terminal_intent')
       r={'ok':True,'value':True}
      elif self.path=='/image-delay':
       global image_delay
       assert isinstance(q['seconds'],int) and 0<=q['seconds']<=40;image_delay=q['seconds'];r={'ok':True,'value':True}
      elif self.path=='/checked':
       assert q['op'] in ['extensions.command','extensions.clientContext','extensions.clientStorage.write','extensions.clientFault'];r={'ok':True,'value':request(q['op'],**installed[q['plugin']],instance=q['instance'],**q.get('arguments',{}))}
      elif self.path=='/audit':
       with sqlite3.connect(home/'data/conversations.sqlite') as sql:
        if q.get('deny'):sql.execute("CREATE TRIGGER deny_live_settings BEFORE INSERT ON extension_calls WHEN NEW.method='settings.apply' BEGIN SELECT RAISE(ABORT,'owned live settings audit failure');END")
        else:sql.execute('DROP TRIGGER deny_live_settings')
       r={'ok':True,'value':True}
      elif self.path=='/replay-consent':
       global fixture_replay_consent
       assert os.environ.get('DUNLIN_WORKBENCH_REPLAY');fixture_replay_consent=q.get('allow') is True;r={'ok':True,'value':True}
      elif self.path=='/consent':
       global fixture_consent
       fixture_consent=q.get('allow') is True;r={'ok':True,'value':True}
      elif self.path=='/quiesce-with-worker':
       worker=lib.dc_open_agent_worker(db);assert worker
       try:r={'ok':True,'value':request('extensions.quiesce')}
       finally:lib.dc_close(worker)
      elif self.path=='/quiesce-audit':
       with sqlite3.connect(home/'data/conversations.sqlite') as sql:
        sql.execute('DROP TRIGGER IF EXISTS deny_shutdown_receipt')
        if q.get('deny'):sql.execute("CREATE TRIGGER deny_shutdown_receipt BEFORE UPDATE ON extension_calls WHEN NEW.method='plugins.quiesce' AND NEW.status='reported' BEGIN SELECT RAISE(ABORT,'owned shutdown receipt denial');END")
       r={'ok':True,'value':True}
      elif self.path=='/manage':
       assert q['op'] in ['extensions.list','extensions.contributions','extensions.themes','extensions.presentation.read','extensions.presentation.write','extensions.faults','extensions.clients','extensions.bind','extensions.uninstall','extensions.clientFault','extensions.config.get','extensions.quiesce','extensions.assets','extensions.restore'];r={'ok':True,'value':ok(q['op'],**(dict(installed[q['plugin']],**q.get('arguments',{})) if 'plugin' in q else q.get('arguments',{})))}
      elif self.path=='/tamper':
       assert q['plugin']=='dsh-better-sidebar'
       with sqlite3.connect(home/'data/conversations.sqlite') as sql:path=pathlib.Path(sql.execute('SELECT path FROM extension_versions WHERE source=? AND package=? AND version=?',tuple(installed[q['plugin']][k] for k in ['source','package','version'])).fetchone()[0])
       (path/'lib/client-editor.js').write_text('throw new Error("owned tamper must never execute")');r={'ok':True,'value':True}
      elif self.path=='/model':
       cid=q['chat'];assert cid in ok('extensions.clientContext',**next(iter(installed.values())),chat='ui-chat')['sessions']['byId'];pointer=lib.dc_run(db,cid.encode(),cfg,q.get('input','Owned context verification').encode(),None);assert pointer
       try:r={'ok':True,'value':{'run':json.loads(C.string_at(pointer)),'request':model_requests[-1]}}
       finally:lib.dc_free(pointer)
      elif self.path=='/fixture':
       assert q['write'] in ['checkpoint-changed.txt'];(work/q['write']).write_text(q['text']);r={'ok':True,'value':True}
      elif self.path=='/inspect':
       with sqlite3.connect(home/'data/conversations.sqlite') as sql:
        pending=[dict(zip(['id','call','package','surface','chat','state','callStatus'],row)) for row in sql.execute('SELECT r.id,r.call,r.package,r.surface,r.chat,r.state,c.status FROM extension_requests r JOIN extension_calls c ON c.id=r.call')]
       r={'ok':True,'value':{'projectCount':sql.execute('SELECT count(*) FROM projects').fetchone()[0],'presentationRows':sql.execute('SELECT count(*) FROM extension_presentations').fetchone()[0] if sql.execute("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='extension_presentations'").fetchone()[0] else 0,'shutdownCalls':[dict(zip(['package','status'],row)) for row in sql.execute("SELECT package,status FROM extension_calls WHERE method='plugins.quiesce' ORDER BY rowid")],'calls':calls,'requests':pending,'imageRequests':image_requests,'clipboardWrites':clipboard_writes,'files':{p.name:p.read_text() for p in work.iterdir() if p.is_file() and p.suffix=='.txt'},'context':request('extensions.clientContext',**next(iter(installed.values())),chat='ui-chat').get('data')}}
      else:raise ValueError('Unknown test owner route')
    except Exception as error:r={'ok':False,'error':str(error),**({'failure':error.response} if isinstance(error,RequestError) else {})}
    data=json.dumps(r).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
  class OwnerServer(http.server.ThreadingHTTPServer):
   daemon_threads=False
   request_queue_size=32
  server=OwnerServer(('127.0.0.1',0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
  print(json.dumps({'port':server.server_port,'token':token,'clients':clients,'workspace':str(work),'imageProvider':f'http://127.0.0.1:{model.server_port}/v1'}),flush=True);sys.stdin.readline();server.shutdown();server.server_close()
 finally:
  for thread in native_threads:thread.join()
  if db:lib.dc_close(db)
  model.shutdown();model.server_close()
