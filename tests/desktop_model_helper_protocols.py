#!/usr/bin/env python3
"""Real C helper HTTP/secret/options boundary with privileged owned audit fixtures.

This constructs private admission rows; original SDK/native bridge authority is
tested separately. No Node/model tools, user keys or external providers.
"""
import ctypes as C,hashlib,http.server,importlib.util,json,pathlib,sqlite3,sys,tempfile,threading,platform,datetime
library=pathlib.Path(sys.argv[1]).resolve();report=pathlib.Path(sys.argv[2]);report.mkdir(parents=True)
inputs=[pathlib.Path(__file__),pathlib.Path('tests/protocol_e2e.py'),library,library.parent/'inputs.json'];(report/'test-inputs.json').write_text(json.dumps({'at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'environment':platform.platform(),'command':sys.argv,'inputs':[{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in inputs]},indent=2)+'\n')
spec=importlib.util.spec_from_file_location('protocol_fixtures',pathlib.Path('tests/protocol_e2e.py'));fixtures=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixtures)
lib=C.CDLL(str(library));P=C.c_void_p;S=C.c_char_p
for name,args,result in [('dc_open',[S],P),('dc_open_agent_worker',[P],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_workspace_request',[P,S],P),('dc_helper_execute',[P,S,C.c_longlong,S,P],P),('dc_helpers_plan',[P,S,P],P),('dc_helpers_native',[P,S,P,S],P),('dc_helpers_handoff',[P,S],C.c_int),('dunlin_json_parse',[S,C.c_size_t],P),('json_object_to_json_string_ext',[P,C.c_int],S),('json_object_put',[P],C.c_int)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=result
def encoded(v):return json.dumps(v,ensure_ascii=False).encode()
def cjson(v):data=encoded(v);return lib.dunlin_json_parse(data,len(data))
def json_result(p):
 assert p
 try:return json.loads(lib.json_object_to_json_string_ext(p,0))
 finally:lib.json_object_put(p)
def raw_result(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
def request(s,op,**v):
 result=raw_result(lib.dc_workspace_request(s,encoded(dict(op=op,**v))));assert result['ok'],result;return result['data']
system='你是任务标题生成器。把用户给的任务说明总结成一条简短标题：不超过 40 个汉字（英文按词），只输出标题本身，不要引号、不要句末句号、不要解释、不要 Markdown。'
secret='owned-helper-protocol-fixture-key'
results=[]
for protocol in ['chat','responses','anthropic','ollama','gemini']:
 for none in [False,True]:
  state={'requests':[],'failures':[]}
  class Handler(http.server.BaseHTTPRequestHandler):
   def log_message(self,*a):pass
   def do_POST(self):
    try:
     body=json.loads(self.rfile.read(int(self.headers['Content-Length'])));state['requests'].append(body)
     assert self.path=={'chat':'/v1/chat/completions','responses':'/v1/responses','anthropic':'/v1/messages','ollama':'/api/chat','gemini':'/v1/models/fixture:streamGenerateContent?alt=sse'}[protocol],self.path
     assert self.headers.get('x-api-key' if protocol=='anthropic' else 'x-goog-api-key' if protocol=='gemini' else 'Authorization')==(secret if protocol in ['anthropic','gemini'] else 'Bearer '+secret)
     if protocol in ['anthropic','gemini']:assert not self.headers.get('Authorization')
     if protocol=='anthropic':assert self.headers['anthropic-version']=='2026-10-21'
     assert not body.get('tools') and not body.get('tool_choice'),'text helper obtained model tools'
     assert secret not in json.dumps(body) and 'Owned ordinary chat must not enter helpers' not in json.dumps(body)
     if protocol=='chat':
      assert body['max_tokens']==256 and len(body['messages'])==2
      assert body['messages'][0]['content']==system and body['messages'][1]['content']=='任务说明：\nOwned protocol task'
      if none:assert body['reasoning_effort']=='none' and body['enable_thinking'] is False and body['chat_template_kwargs']=={'chat_template':'owned-template'}
      wire=('data: '+json.dumps({'choices':[{'index':0,'delta':{'content':fixtures.TEXT},'finish_reason':None}]})+'\n\ndata: '+json.dumps({'choices':[{'index':0,'delta':{},'finish_reason':'stop'}]})+'\n\ndata: [DONE]\n\n').encode()
     else:
      wire=fixtures.response_payload(protocol,2,True)
      if protocol=='responses':assert body['max_output_tokens']==256 and body['store'] is False
      if protocol=='anthropic':assert body['max_tokens']==256
      if protocol=='ollama':assert body['options']['num_predict']==256
      if protocol=='gemini':assert body['generationConfig']['maxOutputTokens']==256
     if none:
      if protocol=='responses':assert body['reasoning']['effort']=='none'
      if protocol=='anthropic':assert body['thinking']=={'type':'disabled'}
      if protocol=='ollama':assert body['think'] is False
      if protocol=='gemini':assert body['generationConfig']['thinkingConfig']=={'thinkingBudget':0}
     self.send_response(200);self.send_header('Content-Type','application/x-ndjson' if protocol=='ollama' else 'text/event-stream');self.send_header('Content-Length',str(len(wire)));self.end_headers();self.wfile.write(wire)
    except Exception as error:state['failures'].append(repr(error));self.send_error(500)
  server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
  with tempfile.TemporaryDirectory(prefix='dunlin-helper-protocol-') as tmp:
   home=pathlib.Path(tmp);data=home/'data';data.mkdir(mode=0o700);work=home/'workspace';work.mkdir();store=lib.dc_open(str(data).encode());assert store
   try:
    assert lib.dc_add_chat(store,b'owned-chat',b'Owned ordinary chat must not enter helpers',str(work).encode())
    profile=dict(id='owned-model',name='Owned protocol model',endpoint=f'http://127.0.0.1:{server.server_port}/'+('api' if protocol=='ollama' else 'v1'),model='fixture',protocolName=protocol,maxOutput=256,authentication='auto',credentialRevision=1,contextLimit=8192,contextBudget=8192)
    if none:profile.update(reasoningCapabilities=['none','xhigh'] if protocol=='gemini' else ['none'],reasoningEffort='none')
    if protocol=='chat' and none:profile.update(enableThinking=False,chatTemplate='owned-template',temperature=0.3)
    request(store,'config.save',revision=0,models=[profile],providers=[])
    arg=cjson({})
    try:catalog=json_result(lib.dc_helpers_native(store,b'helper.models',arg,b'owned'))
    finally:lib.json_object_put(arg)
    (report/(protocol+('-none' if none else '')+'-catalog.json')).write_text(json.dumps(catalog,indent=2)+'\n')
    if protocol=='gemini':assert 'xhigh' not in catalog['data'][0].get('capabilities',[]),'unsupported Gemini xhigh offered in selector'
    call='0'*31+'1';sql=sqlite3.connect(data/'conversations.sqlite');before=sql.execute('SELECT state,title,workspace FROM chats WHERE id=\'owned-chat\'').fetchone()
    sql.execute("INSERT INTO extension_versions(source,package,version,digest,manifest,path,files) VALUES('owned','dsh-project-manager','owned','owned','{}',?,'[]')",(str(home),))
    sql.execute("INSERT INTO extension_bindings(source,package,version,enabled,runtime_instance) VALUES('owned','dsh-project-manager','owned',1,'owned')")
    sql.execute("INSERT INTO extension_calls(id,source,package,version,method,args_hash,status) VALUES(?,'owned','dsh-project-manager','owned','http','owned','running')",(call,))
    sql.execute("INSERT INTO extension_native_scopes(call,instance,chat,surface,operation) VALUES(?,'owned','owned-chat','','none')",(call,));sql.commit()
    body=dict(session='owned-chat',text='Owned protocol task',route=dict(provider='dunlin-native',model='owned-model',**({'effort':'none'} if none else {})))
    args=cjson(dict(package='dsh-project-manager',command='http',chat='owned-chat',arguments=dict(method='POST',url='/_dsh/dsh-project-manager/api/goal/title',body=body)))
    try:planned=json_result(lib.dc_helpers_plan(store,call.encode(),args));assert planned['ok'],planned
    finally:lib.json_object_put(args)
    payload=dict(provider='dunlin-native',model='owned-model',maxTokens=400,effort='none' if none else '',purpose='dsh-project-manager:task-title',system=system,session='owned-chat',input='任务说明：\nOwned protocol task',_sequence=1)
    for field,value in [('purpose','foreign:purpose'),('system','foreign system'),('session','foreign-chat'),('input','任务说明：\nForeign task')]:
     wrong=dict(payload,**{field:value});arg=cjson(wrong)
     try:denied=json_result(lib.dc_helpers_native(store,b'helper.submit',arg,call.encode()));assert not denied['ok'],(field,denied)
     finally:lib.json_object_put(arg)
    arg=cjson(payload)
    try:submitted=json_result(lib.dc_helpers_native(store,b'helper.submit',arg,call.encode()));assert submitted['ok'],submitted
    finally:lib.json_object_put(arg)
    job=submitted['data']['id'];sql.execute("INSERT INTO extension_native_effects(call,sequence,method,args_hash,status) VALUES(?,1,'helper.submit','owned','reported')",(call,));sql.execute("INSERT INTO extension_requests(id,call,source,package,version,instance,surface,chat) VALUES('owned',?,'owned','dsh-project-manager','owned','owned','','owned-chat')",(call,));sql.commit();assert lib.dc_helpers_handoff(store,call.encode())
    worker=lib.dc_open_agent_worker(store);assert worker
    try:result=raw_result(lib.dc_helper_execute(worker,job.encode(),1,secret.encode(),None));assert result['ok'],result
    finally:lib.dc_close(worker)
    row=sql.execute('SELECT state,text,error FROM model_helper_jobs WHERE id=?',(job,)).fetchone();assert sql.execute('SELECT state,title,workspace FROM chats WHERE id=\'owned-chat\'').fetchone()==before;sql.close()
    observed=dict(protocol=protocol,none=none,result=result,row=row,requests=state['requests'],failures=state['failures']);(report/(protocol+('-none' if none else '')+'.json')).write_text(json.dumps(observed,ensure_ascii=False,indent=2)+'\n')
    assert not state['failures'],state['failures'];assert len(state['requests'])==1,(protocol,none,observed);assert row==('reported',fixtures.TEXT,''),(protocol,none,row)
    results.append(dict(protocol=protocol,none=none,actualRequests=1,nativeSecretHeader=True,noTools=True,preservedSource=True));print('PASS',protocol,'none' if none else 'default',flush=True)
   finally:lib.dc_close(store);server.shutdown();server.server_close()
(report/'result.json').write_text(json.dumps({'checks':results,'limits':'Privileged owned C audit-row fixtures, real native HTTP/protocol/secret/options and negative submit payloads. Not real optional-host/SDK admission, Keychain, external provider or platform equivalence; those have separate tests.'},indent=2)+'\n')
