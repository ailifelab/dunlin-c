#!/usr/bin/env python3
"""Owned loopback socket test; no real service, audio capture or user secret."""
import ctypes as C, pathlib, sys, socket, threading, hashlib, base64, struct, time
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));libc=C.CDLL(None)
lib.dl_ws_open.argtypes=[C.c_char_p,C.c_char_p,C.c_int,C.c_size_t,C.c_void_p,C.c_void_p];lib.dl_ws_open.restype=C.c_void_p
lib.dl_ws_send.argtypes=[C.c_void_p,C.c_char_p,C.c_size_t];lib.dl_ws_send.restype=C.c_int
lib.dl_ws_receive.argtypes=[C.c_void_p,C.POINTER(C.c_void_p),C.POINTER(C.c_size_t)];lib.dl_ws_receive.restype=C.c_int
lib.dl_ws_close.argtypes=[C.c_void_p];libc.free.argtypes=[C.c_void_p]
failures=[];seen=[]
def exact(c,n):
 out=b''
 while len(out)<n:
  b=c.recv(n-len(out));assert b;out+=b
 return out
def frame(c,op,data,final=True):
 n=len(data);c.sendall(bytes([(128 if final else 0)|op])+ (bytes([n]) if n<126 else b'\x7e'+struct.pack('!H',n))+data)
def serve(listener,mode):
 try:
  c,_=listener.accept();c.settimeout(5)
  with c:
   headers=b''
   while not headers.endswith(b'\r\n\r\n'):headers+=exact(c,1)
   assert b'Authorization: Bearer owned-test-key\r\n' in headers
   key=next(x.split(b':',1)[1].strip() for x in headers.split(b'\r\n') if x.lower().startswith(b'sec-websocket-key:'))
   accept=base64.b64encode(hashlib.sha1(key+b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
   c.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+b'\r\n\r\n')
   head=exact(c,2);assert head[0]==129 and head[1]&128;n=head[1]&127
   if n==126:n=struct.unpack('!H',exact(c,2))[0]
   mask=exact(c,4);payload=exact(c,n);seen.append(bytes(b^mask[i%4] for i,b in enumerate(payload)))
   if mode=='text':
    frame(c,1,b'{"text":"',False);frame(c,9,b'ping');frame(c,0,'你好"}'.encode());time.sleep(.05);frame(c,8,b'')
   elif mode=='binary':frame(c,2,b'unsupported')
   else:frame(c,1,b'x'*5000)
 except BaseException as e:failures.append(repr(e))
 finally:listener.close()
assert not lib.dl_ws_open(b'ws://example.com/test',b'',1,4096,None,None)
assert not lib.dl_ws_open(b'wss://user:secret@example.com/test',b'',0,4096,None,None)
assert not lib.dl_ws_open(b'wss://example.com/test',b'bad\r\nheader',0,4096,None,None)
for mode in ['text','binary','overflow']:
 listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen();thread=threading.Thread(target=serve,args=(listener,mode));thread.start()
 ws=lib.dl_ws_open(f'ws://127.0.0.1:{listener.getsockname()[1]}/owned'.encode(),b'owned-test-key',1,4096,None,None);assert ws
 try:
  outgoing=b'{"type":"owned.fixture"}';assert lib.dl_ws_send(ws,outgoing,len(outgoing))
  end=time.monotonic()+4;result=0
  while time.monotonic()<end and result==0:
   out=C.c_void_p();n=C.c_size_t();result=lib.dl_ws_receive(ws,C.byref(out),C.byref(n))
   if result==1:
    try:assert C.string_at(out,n.value).decode()=='{"text":"你好"}'
    finally:libc.free(out)
   if result==0:time.sleep(.005)
  assert result==(1 if mode=='text' else -1),(mode,result)
 finally:lib.dl_ws_close(ws)
 thread.join(6);assert not thread.is_alive()
assert not failures,failures
assert seen==[b'{"type":"owned.fixture"}']*3
print('PASS actual WebSocket handshake/masking, fragmented Unicode text with ping, binary rejection, size bound and URL/key admission; loopback only')
