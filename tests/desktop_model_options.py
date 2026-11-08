#!/usr/bin/env python3
"""C registry -> exact native core-settings mapper -> actual model protocol wire.
Loopback fixed responses only; no key, provider account or tool approvals.
"""
import desktop_fixture_sqlite as fixture_sqlite
import ctypes as C,json,sys,pathlib,tempfile,http.server,threading,sqlite3
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_workspace_request',[P,S],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_run_with_secret',[P,S,S,S,P,S],P),('dc_read_view',[S,S],P)]:
 fn=getattr(lib,name);fn.argtypes=args;fn.restype=result
def encode(v):return json.dumps(v,ensure_ascii=False).encode()
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
seen=[];errors=[]
class Server(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  try:
   body=json.loads(self.rfile.read(int(self.headers['Content-Length'])));seen.append((self.path,body))
   if self.path.endswith('/responses'):response=dict(id='owned',status='completed',output=[dict(id='message',type='message',role='assistant',status='completed',content=[dict(type='output_text',text='OK',annotations=[])])])
   else:response=dict(choices=[dict(index=0,message=dict(role='assistant',content='OK'),finish_reason='stop')])
   data=encode(response);self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
  except BaseException as e:errors.append(e)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Server);threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='dunlin-model-options-') as tmp:
 root=pathlib.Path(tmp);private=root/'private';private.mkdir(mode=0o700);work=root/'work';work.mkdir();db=lib.dc_open(str(private).encode());assert db
 def req(op,**kw):return value(lib.dc_workspace_request(db,encode(dict(op=op,**kw))))
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r['data']
 base=dict(id='model',name='Fixture model',endpoint=f'http://127.0.0.1:{server.server_port}/v1',model='fixture',protocolName='chat',maxOutput=4096,authentication='none')
 registry=ok('config.save',revision=0,models=[base],providers=[])
 # This pre-existing field combination was accepted but cannot be loaded by
 # the runtime's pinned protocol contract. Registry must reject it at save.
 result=req('config.save',revision=registry['revision'],models=[dict(base,protocolName='anthropic',enableThinking=False)],providers=[])
 assert not result['ok'],('registry accepted unusable non-chat enableThinking option',result)
 def bad(model):
  result=req('config.save',revision=registry['revision'],models=[model],providers=[]);assert not result['ok'],result
 for model in [dict(base,temperature=-0.01),dict(base,temperature=2.01),dict(base,temperature=True),dict(base,temperature='1'),dict(base,protocolName='anthropic',temperature=1),dict(base,protocolName='responses',chatTemplate='template'),dict(base,reasoningEffort='high'),dict(base,reasoningSupported=True,reasoningEffort='high'),dict(base,reasoningEffort='xhigh',reasoningCapabilities=['levels']),dict(base,reasoningEffort='unexpected',reasoningCapabilities=['levels']),dict(base,reasoningCapabilities=['levels','levels']),dict(base,reasoningCapabilities=['invalid']),dict(base,reasoningCapabilities='levels'),dict(base,protocolName='gemini',reasoningEffort='xhigh',reasoningCapabilities=['xhigh'])]:bad(model)
 assert ok('config.list')==registry
 model=dict(base,temperature=0.25,reasoningCapabilities=['none','levels','xhigh'],reasoningEffort='high')
 registry=ok('config.save',revision=registry['revision'],models=[model],providers=[])
 settings=ok('config.model',id='model',revision=registry['revision']);assert settings['temperature']==0.25 and settings['reasoning_effort']=='high' and settings['reasoning_capabilities']==['none','levels','xhigh'] and 'key_env' not in settings
 assert not req('config.model',id='model',revision=registry['revision']-1)['ok']
 assert not req('config.model',id='model',revision='2')['ok']
 assert not req('config.model',id='missing',revision=registry['revision'])['ok']
 readonly=value(lib.dc_read_view(str(private).encode(),encode(dict(op='config.model',id='model',revision=registry['revision']))));assert readonly['data']==settings
 assert lib.dc_add_chat(db,b'chat',b'Chat',str(work).encode());settings.update(stream=False)
 result=value(lib.dc_run_with_secret(db,b'chat',encode(settings),b'Original first message',None,b''));assert result['status']=='completed',result
 assert seen[-1][1]['temperature']==0.25 and seen[-1][1]['reasoning_effort']=='high' and not seen[-1][1].get('tools'),seen[-1]
 model=dict(model,protocolName='responses',reasoningEffort='xhigh',temperature=0.75);registry=ok('config.save',revision=registry['revision'],models=[model],providers=[]);settings=ok('config.model',id='model',revision=registry['revision']);settings['stream']=False
 result=value(lib.dc_run_with_secret(db,b'chat',encode(settings),b'Original second message',None,b''));assert result['status']=='completed',result
 assert seen[-1][0].endswith('/responses') and seen[-1][1]['reasoning']==dict(effort='xhigh') and seen[-1][1]['temperature']==0.75 and 'Original first message' in json.dumps(seen[-1][1]),seen[-1]
 registry=ok('config.save',revision=registry['revision'],models=[base],providers=[]);settings=ok('config.model',id='model',revision=registry['revision']);assert not set(['temperature','reasoning_effort','reasoning_capabilities','enable_thinking','omlx_chat_template'])&set(settings)
 settings['stream']=False;result=value(lib.dc_run_with_secret(db,b'chat',encode(settings),b'Original third message',None,b''));assert result['status']=='completed';assert 'temperature' not in seen[-1][1] and 'reasoning_effort' not in seen[-1][1]
 print('PASS strict option/capability/protocol validation before publication; exact C mapper with revision/identity and read-only guards; actual Chat/Responses temperature/effort wire and default omission, preserving original text across route changes')
 registry=ok('config.capabilities',id='model',revision=registry['revision'],contextLimit=8192,outputLimit=512,source='provider-model-inventory');discovered=registry['models'][0];assert discovered['contextLimit']==8192 and discovered['advertisedOutputLimit']==512 and discovered['maxOutput']==512
 registry=ok('config.capabilities',id='model',revision=registry['revision'],contextLimit=0,outputLimit=0,source='provider-model-inventory');discovered=registry['models'][0];assert 'contextLimit' not in discovered and 'advertisedOutputLimit' not in discovered and 'contextSource' not in discovered
 revision=registry['revision'];result=req('config.capabilities',id='model',revision=revision,contextLimit=512,outputLimit=16,source='provider-model-inventory');assert result['error']=='configuration_capability_conflict' and ok('config.list')==registry,result
 assert not req('config.capabilities',id='model',revision=revision-1,contextLimit=8192,outputLimit=512,source='provider-model-inventory')['ok']
 manual=dict(base,deploymentLimit=2048,maxOutput=256,contextBudget=1500);registry=ok('config.save',revision=revision,models=[manual],providers=[])
 registry=ok('config.capabilities',id='model',revision=registry['revision'],contextLimit=1024,outputLimit=1024,source='provider-input-limit-conservative');discovered=registry['models'][0];assert discovered['maxOutput']==256 and discovered['deploymentLimit']==2048 and 'contextBudget' not in discovered and '保守' in discovered['contextSource'],discovered
 with fixture_sqlite.connect(private/'conversations.sqlite') as sql:
  checkpoint=json.loads(sql.execute("SELECT state FROM chats WHERE id='chat'").fetchone()[0]);checkpoint['active']=True;sql.execute("UPDATE chats SET state=? WHERE id='chat'",(json.dumps(checkpoint),))
 result=req('config.capabilities',id='model',revision=registry['revision'],contextLimit=8192,outputLimit=512,source='provider-model-inventory');assert result['error']=='configuration_active_route' and ok('config.list')==registry,result
 with fixture_sqlite.connect(private/'conversations.sqlite') as sql:sql.execute("UPDATE chat_properties SET model='different-preference' WHERE chat='chat'")
 result=req('config.capabilities',id='model',revision=registry['revision'],contextLimit=8192,outputLimit=512,source='provider-model-inventory');assert result['error']=='configuration_active_route' and ok('config.list')==registry,result
 with fixture_sqlite.connect(private/'conversations.sqlite') as sql:
  checkpoint['active']=False;sql.execute("UPDATE chats SET state=? WHERE id='chat'",(json.dumps(checkpoint),))
 print('PASS capability application clamps output to actual bounded declarations, removes unknown service maxima, preserves manual limits and refuses stale/active/impossible changes without publication')
 source=dict(base_url=base['endpoint'],model='imported',protocol='responses',max_tokens=4096,temperature=0.4,reasoning_effort='xhigh',reasoning_capabilities=['xhigh'])
 plan=ok('config.preview',source=json.dumps(source));mapped=plan['models'][0];assert mapped['temperature']==0.4 and mapped['reasoningEffort']=='xhigh' and mapped['reasoningCapabilities']==['xhigh'],plan
 registry=ok('config.apply',plan=plan['plan'],confirmed=True);lib.dc_close(db);db=lib.dc_open(str(private).encode());assert db;assert ok('config.list')==registry;lib.dc_close(db);assert not errors,errors
 print('PASS explicit static runtime import mapping and restart persistence without inferring capabilities from a reasoning boolean')
server.shutdown();server.server_close()
