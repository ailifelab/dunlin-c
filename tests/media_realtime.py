#!/usr/bin/env python3
"""Actual C Realtime adapter against an owned loopback WebSocket fixture."""
import base64, ctypes as C, hashlib, json, pathlib, socket, struct, sys, threading, time
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
Callback=C.CFUNCTYPE(None,C.c_void_p,C.c_char_p,C.c_void_p,C.c_size_t)
class Hooks(C.Structure):_fields_=[('user',C.c_void_p),('cancelled',C.c_void_p),('event',Callback)]
lib.dl_realtime_open.argtypes=[C.c_char_p,C.c_char_p,C.POINTER(Hooks)];lib.dl_realtime_open.restype=C.c_void_p
for name in ['poll','ready']:
 f=getattr(lib,'dl_realtime_'+name);f.argtypes=[C.c_void_p];f.restype=C.c_int
for name in ['audio','image']:
 f=getattr(lib,'dl_realtime_'+name);f.argtypes=[C.c_void_p,C.c_void_p,C.c_size_t];f.restype=C.c_int
lib.dl_realtime_interrupt.argtypes=[C.c_void_p,C.c_char_p,C.c_uint];lib.dl_realtime_interrupt.restype=C.c_int
lib.dl_realtime_close.argtypes=[C.c_void_p]
def exact(c,n):
 out=b''
 while len(out)<n:
  b=c.recv(n-len(out));assert b;out+=b
 return out
def receive(c):
 h=exact(c,2);assert h[0]==129 and h[1]&128;n=h[1]&127
 if n==126:n=struct.unpack('!H',exact(c,2))[0]
 if n==127:n=struct.unpack('!Q',exact(c,8))[0]
 mask=exact(c,4);data=exact(c,n)
 return json.loads(bytes(x^mask[i%4] for i,x in enumerate(data)))
def send(c,data):
 data=json.dumps(data).encode();n=len(data);c.sendall(b'\x81'+(bytes([n]) if n<126 else b'\x7e'+struct.pack('!H',n))+data)
def run(mode):
 listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen();errors=[];events=[];stop=threading.Event()
 def serve():
  try:
   conn,_=listener.accept();conn.settimeout(5)
   with conn:
    headers=b''
    while not headers.endswith(b'\r\n\r\n'):headers+=exact(conn,1)
    assert b'/owned?model=fixture%20model ' in headers and b'Authorization: Bearer fixture-key\r\n' in headers
    key=next(x.split(b':',1)[1].strip() for x in headers.split(b'\r\n') if x.lower().startswith(b'sec-websocket-key:'))
    accept=base64.b64encode(hashlib.sha1(key+b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
    conn.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+b'\r\n\r\n')
    send(conn,{'type':'session.created'})
    cfg=receive(conn);assert cfg['type']=='session.update';cfg=cfg['session']
    assert cfg['tools']==[] and cfg['output_modalities']==['audio']
    assert cfg['audio']['input']['transcription']['model']=='fixture-asr'
    if mode=='format':cfg['audio']['input']['format']['rate']=16000
    send(conn,{'type':'session.updated','session':cfg})
    if mode!='format':
     for i,expected in enumerate([('user','input_text','保留原始文件。'),('assistant','output_text','我会保留。')]):
      event=receive(conn);assert event['type']=='conversation.item.create';item=event['item'];assert item['role']==expected[0] and item['content']==[dict(type=expected[1],text=expected[2])]
      assert item['id']=='dunlin_history_'+str(i)
      send(conn,dict(type='conversation.item.done',item=item))
     audio=receive(conn);assert audio['type']=='input_audio_buffer.append' and base64.b64decode(audio['audio'])==b'\x01\x00'*4
     truncate=receive(conn);assert truncate=={'type':'conversation.item.truncate','item_id':'out-1','content_index':0,'audio_end_ms':120}
     if mode=='bad_audio':send(conn,{'type':'response.output_audio.delta','item_id':'out-1','delta':'AA=A'})
     else:
      send(conn,{'type':'response.output_audio.delta','item_id':'out-1','delta':base64.b64encode(b'\x02\x00'*4).decode()})
      send(conn,{'type':'response.output_audio_transcript.done','item_id':'out-1','transcript':'您好，不需要删除文件。'})
      send(conn,{'type':'conversation.item.input_audio_transcription.completed','item_id':'in-1','transcript':'保留原始内容。'})
      send(conn,{'type':'conversation.item.input_audio_transcription.failed','item_id':'in-2'})
      send(conn,{'type':'response.done','response':{'status':'completed','output':[{'id':'out-1','type':'message','role':'assistant','content':[{'type':'output_audio','transcript':'您好，不需要删除文件。'}]}]}})
      send(conn,{'type':'response.done','response':{'status':'completed','output':[{'id':'out-2','type':'message','role':'assistant','content':[{'type':'output_audio'}]}]}})
      send(conn,{'type':'response.done','response':{'status':'incomplete','output':[{'id':'out-3','type':'message','role':'assistant','content':[{'type':'output_text','text':'保留的部分回答。'}]}]}})
      send(conn,{'type':'input_audio_buffer.speech_started'})
    stop.wait(4)
  except BaseException as e:errors.append(repr(e))
  finally:listener.close()
 thread=threading.Thread(target=serve);thread.start()
 @Callback
 def callback(user,kind,payload,n):events.append((kind.decode(),C.string_at(payload,n)))
 hooks=Hooks(None,None,callback)
 cfg=json.dumps({'url':f'ws://127.0.0.1:{listener.getsockname()[1]}/owned','model':'fixture model','voice':'fixture','local':True,'transcriptionModel':'fixture-asr','images':False,'history':[dict(role='user',text='保留原始文件。'),dict(role='assistant',text='我会保留。')]}).encode()
 session=lib.dl_realtime_open(cfg,b'fixture-key',C.byref(hooks));assert session
 try:
  assert not lib.dl_realtime_audio(session,b'\0\0',2)
  end=time.monotonic()+4;sent=False
  while time.monotonic()<end:
   ok=lib.dl_realtime_poll(session)
   if lib.dl_realtime_ready(session) and not sent:
    assert not lib.dl_realtime_image(session,b'\xff\xd8\xff\xd9',4)
    assert not lib.dl_realtime_audio(session,b'x',1)
    assert lib.dl_realtime_audio(session,b'\x01\x00'*4,8)
    assert lib.dl_realtime_interrupt(session,b'out-1',120);sent=True
   if not ok or any(k=='interrupted' for k,_ in events):break
   time.sleep(.002)
  if mode=='good':
   assert [k for k,_ in events]==['ready','audio.item','audio','final','final','gap','gap','final','response.failed','interrupted'],events
   assert events[2][1]==b'\x02\x00'*4
   assert json.loads(events[3][1])['text']=='您好，不需要删除文件。'
   assert json.loads(events[4][1])['source']=='provider_asr'
  else:assert events[-1][0]=='error' and not lib.dl_realtime_ready(session),events
 finally:stop.set();lib.dl_realtime_close(session);thread.join(6)
 assert not thread.is_alive() and not errors,errors
for mode in ['good','format','bad_audio']:run(mode)
# Malformed history is rejected before the transport can contact a listener.
listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen();listener.setblocking(False)
for history in ['wrong',[dict(role='tool',text='not projected')],[dict(role='user',text='x'*65537)]]:
 cfg=json.dumps(dict(url=f'ws://127.0.0.1:{listener.getsockname()[1]}/owned',model='fixture',voice='fixture',local=True,history=history)).encode()
 assert not lib.dl_realtime_open(cfg,b'',None)
try:
 c,_=listener.accept();c.close();raise AssertionError('invalid context contacted transport')
except BlockingIOError:pass
finally:listener.close()
print('PASS C Realtime negotiation, PCM transfer, exact final transcripts, ASR gap, interruption, camera admission and malformed audio rejection; owned loopback fixture only')
