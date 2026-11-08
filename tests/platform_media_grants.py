#!/usr/bin/env python3
"""Owned C call/permission integration. Never invokes an OS reset command."""
import ctypes as C,json,pathlib,sys,tempfile
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for name,args,out in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_bind_identity',[P,S],C.c_int),('dc_configure_platform_grants',[P,S],C.c_int),('dc_workspace_request',[P,S],P)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=out
with tempfile.TemporaryDirectory(prefix='dunlin-call-grants-') as temp:
 path=pathlib.Path(temp);path.chmod(0o700);store=lib.dc_open(str(path).encode());assert store
 try:
  def req(op,**values):
   raw=lib.dc_workspace_request(store,json.dumps(dict(op=op,**values)).encode());assert raw
   try:return json.loads(C.string_at(raw))
   finally:lib.dc_free(raw)
  assert lib.dc_bind_identity(store,b'owned-principal')
  assert lib.dc_configure_platform_grants(store,b'org.dunlin.owned.fixture')
  assert lib.dc_add_chat(store,b'chat',b'Owned call',b'')
  assert req('media.start',id='call',chat='chat',mode='voice',model='fixture',saveText=True)['ok']
  assert req('grants.platform.record',call='call',resource='speech')['ok']
  assert not req('grants.platform.record',call='call',resource='all')['ok']
  assert not req('grants.platform.record',call='other',resource='speech')['ok']
  assert req('grants.platform.revoke',id='call-speech')['error']=='stop_media_before_revoking'
  assert req('media.state',id='call',state='live')['ok']
  assert req('media.append',id='call',item='recognized',role='user',kind='final',source='local_asr',text='本地识别结果，需核对。')['ok']
  assert req('media.items',id='call')['data'][0]['source']=='local_asr'
  assert req('media.state',id='call',state='ended')['ok']
  result=req('grants.platform.revoke',id='call-speech');assert result['ok'],result
  assert result['data'][0]['state']=='needs_user',result
  assert req('grants.platform.list')['data'][0]['chat']=='chat'
 finally:lib.dc_close(store)
print('PASS call-scoped Speech permission record, invalid/foreign resource rejection, active-call revoke barrier, explicit manual OS handoff and local-ASR source label; no microphone/Speech recognition or OS reset performed')
