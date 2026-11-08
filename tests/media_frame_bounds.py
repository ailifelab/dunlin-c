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
  assert lib.dc_add_chat(store,b'chat-1',b'Long call',b'')
  assert request('media.start',id='frames',chat='chat-1',mode='video',model='fixture',saveText=True,saveFrames=True,retentionDays=1)['ok']
  assert request('media.state',id='frames',state='live')['ok']
  for i in range(361):assert stage('frames',i*10000)['ok']
  assert not stage('frames',3610000)['ok'],'one-hour frame bound bypassed'
  rows=request('media.frames',id='frames')['data'];assert len(rows)==361
  assert [r['milliseconds'] for r in rows]==list(range(0,3600001,10000))
  assert base64.b64decode(request('media.frame.read',id='frames',sequence=rows[-1]['sequence'])['data']['png'])==png
  print('PASS full one-hour frame range listed without loss, beyond-limit frame rejected, final frame readable')

 finally:lib.dc_close(store)
