#!/usr/bin/env python3
"""Real C media source transaction and next model turn, owned HTTP fixture only."""
import ctypes as C,http.server,json,pathlib,sqlite3,sys,tempfile,threading
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_messages',[P,S],P),('dc_workspace_request',[P,S],P),('dc_run_message_with_secret',[P,S,S,S,P,S,S,C.POINTER(C.c_int)],P),('dc_index_pending',[P,C.c_uint],C.c_int)]:
 fn=getattr(lib,name);fn.argtypes=args;fn.restype=result
requests=[]
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  requests.append(json.loads(self.rfile.read(int(self.headers['Content-Length']))))
  data=json.dumps({'choices':[{'index':0,'message':{'role':'assistant','content':'fixture response'},'finish_reason':'stop'}]}).encode()
  self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
def value(raw):
 assert raw
 try:return json.loads(C.string_at(raw))
 finally:lib.dc_free(raw)
try:
 with tempfile.TemporaryDirectory(prefix='dunlin-media-context-') as root:
  pathlib.Path(root).chmod(0o700);workspace=pathlib.Path(root)/'project';workspace.mkdir();data=pathlib.Path(root)/'private';data.mkdir(mode=0o700);store=lib.dc_open(str(data).encode());assert store
  try:
   def req(op,**kw):return value(lib.dc_workspace_request(store,json.dumps(dict(op=op,**kw)).encode()))
   assert lib.dc_add_chat(store,b'chat',b'Owned media context',str(workspace).encode())
   assert req('media.start',id='call',chat='chat',mode='voice',model='media-fixture',saveText=True)['ok']
   assert req('media.context',id='call')['data']==[]
   assert not req('media.context',id='foreign')['ok']
   assert req('media.state',id='call',state='live')['ok']
   args=dict(id='call',item='user-1',role='user',kind='final',source='provider_asr',text='不要删除 greeting.txt。')
   assert req('media.append',**args)['ok'];assert req('media.append',**args)['ok']
   assert req('media.append',**dict(args,item='assistant-1',role='assistant',source='provider_text',text='我会保留这个文件。'))['ok']
   original=value(lib.dc_messages(store,b'chat'));assert len(original)==3 and original[1]['media']['source']=='provider_asr'
   assert original[0]['media']['kind']=='start'
   sql=sqlite3.connect(data/'conversations.sqlite');sql.create_function('dunlin_workspace_writer_v1',0,lambda:1)
   sql.execute("CREATE TRIGGER owned_media_failure BEFORE INSERT ON index_jobs BEGIN SELECT RAISE(ABORT,'owned failure'); END");sql.commit()
   assert not req('media.append',**dict(args,item='rollback'))['ok']
   assert len(req('media.items',id='call')['data'])==2 and value(lib.dc_messages(store,b'chat'))==original
   sql.execute('DROP TRIGGER owned_media_failure');sql.commit();sql.close()
   assert req('media.state',id='call',state='ended')['ok']
   config=dict(base_url=f'http://127.0.0.1:{server.server_port}/v1',model='text-fixture-a',protocol='chat',stream=False,max_tokens=256,desktop_tools='noTools')
   for model in ['text-fixture-a','text-fixture-b']:
    config['model']=model;committed=C.c_int(0)
    result=value(lib.dc_run_message_with_secret(store,b'chat',json.dumps(config).encode(),'继续确认刚才的约定。'.encode(),None,b'',None,C.byref(committed)))
    assert result['status']=='completed' and committed.value==1,result
    assert any(m.get('content')=='不要删除 greeting.txt。' for m in requests[-1]['messages'])
    assert not any(any(k.startswith('_dunlin_') for k in m) for m in requests[-1]['messages'])
    assert value(lib.dc_messages(store,b'chat'))[:3]==original
   assert req('media.start',id='next-call',chat='chat',mode='voice',model='other-media',saveText=True)['ok']
   history=req('media.context',id='next-call');assert history['ok'],history
   assert any(m==dict(role='user',text='不要删除 greeting.txt。') for m in history['data'])
   assert any(m==dict(role='assistant',text='我会保留这个文件。') for m in history['data'])
   assert req('media.state',id='next-call',state='ended')['ok']
   assert req('media.start',id='large-source',chat='chat',mode='voice',model='media',saveText=True)['ok']
   assert req('media.state',id='large-source',state='live')['ok']
   assert req('media.append',id='large-source',item='large',role='user',kind='final',source='provider_asr',text='x'*65520)['ok']
   assert req('media.state',id='large-source',state='ended')['ok']
   assert req('media.start',id='over-budget',chat='chat',mode='voice',model='media',saveText=True)['ok']
   assert req('media.context',id='over-budget')['error']=='media_context_too_large'
   assert value(lib.dc_messages(store,b'chat'))[-1]['content']=='x'*65520
   assert lib.dc_index_pending(store,32)>=0
  finally:lib.dc_close(store)
finally:server.shutdown();server.server_close();thread.join()
print('PASS exact media source/index atomicity and rollback, duplicate receipt, ordinary model turn and model-switch context retention; owned protocol fixture, no live provider')
