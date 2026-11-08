#!/usr/bin/env python3
"""C-owned call records; isolated database, no device/provider access."""
import base64,ctypes as C,json,pathlib,sqlite3,struct,sys,tempfile,zlib
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
lib.dc_open.argtypes=[C.c_char_p];lib.dc_open.restype=C.c_void_p
lib.dc_close.argtypes=[C.c_void_p];lib.dc_free.argtypes=[C.c_void_p]
lib.dc_add_chat.argtypes=[C.c_void_p,C.c_char_p,C.c_char_p,C.c_char_p];lib.dc_add_chat.restype=C.c_int
lib.dc_delete_chat.argtypes=[C.c_void_p,C.c_char_p];lib.dc_delete_chat.restype=C.c_int
lib.dc_workspace_request.argtypes=[C.c_void_p,C.c_char_p];lib.dc_workspace_request.restype=C.c_void_p
lib.dc_media_frame_stage.argtypes=[C.c_void_p,C.c_char_p,C.c_void_p,C.c_size_t,C.c_longlong];lib.dc_media_frame_stage.restype=C.c_void_p
def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1,1,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0\x17\x2a\x63\xff'))+chunk(b'IEND',b'')
with tempfile.TemporaryDirectory(prefix='dunlin-media-') as root:
 pathlib.Path(root).chmod(0o700);store=lib.dc_open(root.encode());assert store
 def request(op,**kw):
  p=lib.dc_workspace_request(store,json.dumps(dict(op=op,**kw)).encode());assert p
  try:return json.loads(C.string_at(p))
  finally:lib.dc_free(p)
 def stage(call,stamp,payload=png):
  p=lib.dc_media_frame_stage(store,call.encode(),payload,len(payload),stamp);assert p
  try:return json.loads(C.string_at(p))
  finally:lib.dc_free(p)
 def call(id='call-1',save=True):return request('media.start',id=id,chat='chat-1',mode='voice',model='fixture',saveText=save)
 try:
  assert lib.dc_add_chat(store,b'chat-1',b'Call records',b'')
  assert call()['ok'];assert not call('call-2')['ok'];assert not lib.dc_delete_chat(store,b'chat-1')
  args=dict(id='call-1',item='out-1',role='assistant',kind='final',source='provider_text',text='不要删除原文。')
  assert not request('media.append',**args)['ok']
  assert request('media.state',id='call-1',state='live')['ok']
  assert request('media.append',**args)['ok'];assert request('media.append',**args)['ok']
  assert not request('media.append',**dict(args,text='删除原文。'))['ok']
  assert not request('media.append',**dict(args,kind='partial'))['ok']
  assert request('media.append',**dict(args,item='in-1',role='user',kind='gap',source='provider_asr',text=''))['ok']
  rows=request('media.items',id='call-1')['data'];assert len(rows)==2 and rows[0]['text']=='不要删除原文。'
  assert len(request('media.items',id='call-1',after=rows[0]['sequence'])['data'])==1
  assert request('media.state',id='call-1',state='ended')['ok']
  assert not request('media.append',**dict(args,item='late'))['ok']
  assert not request('media.state',id='call-1',state='live')['ok']
  assert call('private',False)['ok'];assert request('media.state',id='private',state='live')['ok']
  assert request('media.append',**dict(args,id='private'))['ok'];assert request('media.items',id='private')['data']==[]
  lib.dc_close(store);store=lib.dc_open(root.encode());assert store
  assert request('media.list',chat='chat-1')['data'][0]['state']=='interrupted'
  assert not stage('call-1',0)['ok']
  for id in ['frames-a','frames-b']:
   assert request('media.start',id=id,chat='chat-1',mode='video',model='fixture',saveText=True,saveFrames=True,retentionDays=1)['ok']
   assert not stage(id,0)['ok']
   assert request('media.state',id=id,state='live')['ok']
   assert not stage(id,0,b'broken')['ok']
   assert stage(id,0)['ok'];assert not stage(id,9999)['ok'];assert stage(id,10000)['ok']
   rows=request('media.frames',id=id)['data'];assert len(rows)==2
   assert base64.b64decode(request('media.frame.read',id=id,sequence=rows[0]['sequence'])['data']['png'])==png
   assert not request('media.frame.read',id='call-1',sequence=rows[0]['sequence'])['ok']
   assert request('media.state',id=id,state='ended')['ok'];assert not stage(id,20000)['ok']
  sql=sqlite3.connect(pathlib.Path(root)/'conversations.sqlite')
  sql.execute("UPDATE media_calls SET ended=unixepoch()-86401 WHERE id='frames-a'");sql.commit()
  assert request('media.frames',id='frames-a')['data']==[]
  assert request('media.collect')['ok']
  assert sql.execute("SELECT count(*) FROM media_frames WHERE call='frames-a'").fetchone()[0]==0
  assert sql.execute("SELECT count(*) FROM attachment_resources").fetchone()[0]==1
  assert len(request('media.frames',id='frames-b')['data'])==2
  assert lib.dc_delete_chat(store,b'chat-1')
  assert sql.execute("SELECT count(*) FROM attachment_resources").fetchone()[0]==0
  sql.close()
  assert request('media.items',id='call-1')['data']==[] and request('media.list',chat='chat-1')['data']==[]
 finally:lib.dc_close(store)
print('PASS optional keyframe PNG validation/rate/owner isolation/expiry/shared-byte retention/deletion;  C call lifecycle, exact final deduplication/conflict, no partial persistence, explicit gaps, save-off, restart interruption and chat deletion cascade')
