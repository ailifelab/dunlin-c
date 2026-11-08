#!/usr/bin/env python3
import ctypes as C,json,pathlib,sqlite3,sys,tempfile
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p;U=C.c_ulonglong
VISIT=C.CFUNCTYPE(C.c_int,P,C.c_int,U,P,C.c_size_t)
for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_delete_chat',[P,S],C.c_int),('dc_workspace_request',[P,S],P),('dc_media_recording_open',[P,S,U],P),('dc_media_recording_append',[P,C.c_int,U,P,C.c_size_t],C.c_int),('dc_media_recording_finish',[P,P],P),('dc_media_recording_read_open',[P,S],P),('dc_media_recording_scan',[P,VISIT,P],C.c_int),('dc_media_recording_read_close',[P],None)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=result
with tempfile.TemporaryDirectory(prefix='dunlin-recording-') as tmp:
 root=pathlib.Path(tmp);root.chmod(0o700);store=lib.dc_open(str(root).encode());assert store
 def decode(p):
  assert p
  try:return json.loads(C.string_at(p))
  finally:lib.dc_free(p)
 def req(op,**kw):
  r=decode(lib.dc_workspace_request(store,json.dumps(dict(op=op,**kw)).encode()));assert r['ok'],r;return r.get('data')
 def start(call,save=True):req('media.start',id=call,chat='owned-chat',mode='video',model='fixture',saveText=False,saveOriginal=save)
 def end(call):req('media.state',id=call,state='ended')
 def play(call):
  replay=lib.dc_media_recording_read_open(store,call.encode());assert replay
  rows=[]
  @VISIT
  def visit(_,track,stamp,p,n):rows.append((track,stamp,C.string_at(p,n)));return 1
  try:return lib.dc_media_recording_scan(replay,visit,None),rows
  finally:lib.dc_media_recording_read_close(replay)
 try:
  assert lib.dc_add_chat(store,b'owned-chat',b'Owned recording',b'')
  start('private',False);assert not lib.dc_media_recording_open(store,b'private',4096);assert not (root/'media-recordings').exists();end('private')
  start('saved');capture=lib.dc_media_recording_open(store,b'saved',100);assert capture;assert not lib.dc_media_recording_open(store,b'saved',100)
  assert not lib.dc_media_recording_read_open(store,b'saved');assert not lib.dc_delete_chat(store,b'owned-chat')
  pcm=b'\x00\x00\xff\x7f';assert lib.dc_media_recording_append(capture,1,1,pcm,len(pcm))==0
  assert lib.dc_media_recording_append(capture,2,0,pcm,len(pcm))==0
  assert lib.dc_media_recording_append(capture,1,2,pcm,len(pcm))==3
  result=decode(lib.dc_media_recording_finish(store,capture));assert result['ok'] and result['data']['state']=='partial';end('saved')
  code,rows=play('saved');assert code==0 and rows==[(1,1,pcm),(2,0,pcm)]
  assert req('media.recording',id='saved')['state']=='partial'
  files=list((root/'media-recordings').glob('*.dcr'));assert len(files)==1;assert files[0].stat().st_mode&0o777==0o600
  with files[0].open('r+b') as f:f.seek(40);f.write(b'\x01')
  assert play('saved')[0]==4
  sql=sqlite3.connect(root/'conversations.sqlite');sql.execute("UPDATE media_calls SET ended=unixepoch()-31*86400 WHERE id='saved'");sql.commit();sql.close()
  assert not lib.dc_media_recording_read_open(store,b'saved');req('media.collect');assert not files[0].exists()
  start('complete');capture=lib.dc_media_recording_open(store,b'complete',4096);assert capture;assert lib.dc_media_recording_append(capture,1,0,pcm,len(pcm))==0
  assert decode(lib.dc_media_recording_finish(store,capture))['data']['state']=='ready';end('complete');assert play('complete')[0]==0
  start('another');capture=lib.dc_media_recording_open(store,b'another',4096);assert capture
  assert lib.dc_media_recording_append(capture,1,3600001,pcm,len(pcm))==1
  assert decode(lib.dc_media_recording_finish(store,capture))['data']['state']=='partial';end('another')
  assert len(list((root/'media-recordings').glob('*.dcr')))==2
  assert lib.dc_delete_chat(store,b'owned-chat');assert not list((root/'media-recordings').glob('*.dcr'))
  print('PASS explicit opt-in, exclusive active capture, partial quota result, exact PCM replay, corruption detection, expiry admission/cleanup and chat deletion; no devices or provider used.')
 finally:lib.dc_close(store)
