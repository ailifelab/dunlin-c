"""SDK tool definitions -> C model schemas/approval and immutable dispatch.
Real SDK and actual owned effects; HTTP model is deterministic, approvals deny.
"""
import ctypes as C,json,pathlib,tempfile,sys,http.server,threading,sqlite3
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
Schema=C.CFUNCTYPE(P,P);Check=C.CFUNCTYPE(C.c_int,P,S,P,C.POINTER(C.c_int),C.POINTER(C.c_int),P,C.c_size_t);Call=C.CFUNCTYPE(C.c_int,P,S,P,P,C.c_size_t,C.c_uint,C.POINTER(P))
class Tools(C.Structure):_fields_=[('ctx',P),('schema',Schema),('check',Check),('call',Call)]
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_run',[P,S,S,S,P],P),('dc_extension_tools_snapshot',[P],P),('dc_extension_tools_open',[P,S,C.POINTER(Tools),P,C.POINTER(Tools)],C.c_int),('dc_extension_tools_close',[C.POINTER(Tools)],None),('dc_project_tools_open',[C.POINTER(Tools),S,S],C.c_int),('dc_project_tools_close',[C.POINTER(Tools)],None),('json_tokener_parse',[S],P),('json_object_put',[P],C.c_int),('json_object_to_json_string_ext',[P,C.c_int],S)]:
 f=getattr(lib,n);f.argtypes=a;f.restype=r
encode=lambda j:json.dumps(j).encode()
def take(ptr):
 assert ptr
 try:return json.loads(C.string_at(ptr))
 finally:lib.dc_free(ptr)
def jsonvalue(ptr):return json.loads(lib.json_object_to_json_string_ext(ptr,0))
Event=C.CFUNCTYPE(None,P,S,S);Approve=C.CFUNCTYPE(C.c_int,P,S,S);Cancel=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',Event),('approve',Approve),('cancelled',Cancel)]
approvals=[]
@Event
def event(*args):pass
@Approve
def deny(_,name,args):approvals.append((name.decode(),json.loads(args)));return 0
@Cancel
def cancel(_):return 0
hooks=Hooks(None,event,deny,cancel);requests=[];alias=None
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  value=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(value)
  message=dict(role='assistant',content=None,tool_calls=[dict(id='owned-plugin-call',type='function',function=dict(name=alias,arguments=json.dumps({'text':'must be denied'})))])
  finish='tool_calls'
  if value['messages'][-1].get('content')=='policy probe':message=dict(role='assistant',content='policy respected');finish='stop'
  raw=encode(dict(choices=[dict(index=0,message=message,finish_reason=finish)]))
  self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
try:
 with tempfile.TemporaryDirectory(prefix='dunlin-sdk-model-tools-') as tmp:
  home=pathlib.Path(tmp);state=home/'state';state.mkdir(mode=0o700);work=home/'workspace';work.mkdir();effect=work/'effect.txt';package=home/'package';package.mkdir();db=lib.dc_open(str(state).encode());assert db
  def req(op,**kw):return take(lib.dc_extensions_call(db,encode(dict(op=op,**kw)),None))
  def ok(op,**kw):
   r=req(op,**kw);assert r['ok'],r;return r['data']
  try:
   ok('extensions.assets',path=str(pathlib.Path(sys.argv[2]).resolve()));ok('compat.enable',node='/opt/homebrew/bin/node')
   manifest=dict(protocol='dunlin.extension',major=1,packageId='owned-model-tool',version='1',kind='plugin',engines={'dunlin':'desktop@1','host':'node-dsh','dsh':'0.2.1-rc.3'},entry='index.ts',capabilities=['trusted-full-host'],requires=[],dependencies=[])
   (package/'index.ts').write_text("import {writeFile} from 'node:fs/promises';import {defineTool} from '@deepseek-ai/dsh-tools';export const inject=['tools'];export function apply(ctx,config){ctx.tools.register(defineTool({name:'record_note',description:'Owned acceptance note',parameters:{text:{type:'string',required:true}},output:{schema:{type:'string'},render:(_args,value)=>[{type:'text',text:value}]},async execute(args){if(args.text==='throw')throw Error('private payload must not enter diagnostics');await writeFile(config.target,args.text);return 'persisted '+args.text}}))}")
   def install(version):
    manifest['version']=version;(package/'dunlin.json').write_text(json.dumps(manifest));row=ok('extensions.import',path=str(package))[0];ref={k:row[k] for k in ('source','package')};ok('extensions.bind',**ref,version=version,enabled=True,acceptHostPermissions=True,config={'target':str(effect)});return ref
   ref=install('1');assert lib.dc_add_chat(db,b'owned-chat',b'Owned model tool',str(work).encode());base=Tools();assert lib.dc_project_tools_open(C.byref(base),str(work).encode(),b'ask')
   snapshot=lib.dc_extension_tools_snapshot(db);assert snapshot;rows=jsonvalue(snapshot);assert len(rows)==1 and len(rows[0]['configurationHash'])==64,rows
   service=Tools();assert lib.dc_extension_tools_open(db,b'owned-chat',C.byref(base),snapshot,C.byref(service));lib.json_object_put(snapshot)
   schema_ptr=service.schema(service.ctx);schemas=jsonvalue(schema_ptr);lib.json_object_put(schema_ptr);alias=next(x['function']['name'] for x in schemas if x['function']['name'].endswith('_record_note'))
   args=lib.json_tokener_parse(b'{"text":"manual fixture effect"}');approval=C.c_int();effect_flag=C.c_int();error=C.create_string_buffer(512)
   assert service.check(service.ctx,alias.encode(),args,C.byref(approval),C.byref(effect_flag),error,512)==0 and approval.value==1 and effect_flag.value==1
   # Direct fixture invocation verifies dispatch; no LLM request or approval is simulated.
   result=P();assert service.call(service.ctx,alias.encode(),args,None,65536,10000,C.byref(result))==0;assert not take(result)['isError'];assert effect.read_text()=='manual fixture effect';effect.unlink();lib.json_object_put(args)
   settings=dict(base_url=f'http://127.0.0.1:{server.server_port}/v1',model='owned',protocol='chat',stream=False,desktop_tools='ask',max_rounds=3)
   response=take(lib.dc_run(db,b'owned-chat',encode(settings),b'owned denied tool',C.byref(hooks)));assert response['status']!='completed',response;assert not effect.exists();assert approvals and approvals[-1][0]==alias,(response,requests,approvals);assert any(t['function']['name']==alias for t in requests[-1]['tools'])
   # Update cannot retarget an already captured call to new code/config.
   for policy in ['readOnly','noTools']:
    chat=('policy-'+policy).encode();assert lib.dc_add_chat(db,chat,b'Owned policy',str(work).encode());reply=take(lib.dc_run(db,chat,encode(dict(settings,desktop_tools=policy)),b'policy probe',C.byref(hooks)));assert reply['status']=='completed',reply;assert not any(t['function']['name'].startswith('ext_') for t in requests[-1].get('tools',[]));assert not effect.exists()
   install('2');args=lib.json_tokener_parse(b'{"text":"stale must not run"}');result=P();status=service.call(service.ctx,alias.encode(),args,None,65536,10000,C.byref(result));assert status!=0;assert not effect.exists();assert 'extension_version_changed' in json.dumps(take(result));lib.json_object_put(args)
   lib.dc_extension_tools_close(C.byref(service));lib.dc_project_tools_close(C.byref(base));failure=req('extensions.command',**ref,version='2',chat='owned-chat',command='tool:record_note',arguments={'text':'throw'});assert not failure['ok'];assert not ok('extensions.list')[0]['active'];faults=ok('extensions.faults');assert faults and 'private payload' not in json.dumps(faults);ok('extensions.uninstall',**ref);assert ok('extensions.tools')==[]
  finally:lib.dc_close(db)
finally:server.shutdown();server.server_close()
print('PASS actual SDK schema -> C model request, explicit effectful approval check, denied model dispatch has no file effect, direct owned dispatch writes, old version cannot execute after update/uninstall. No live model approval.')
