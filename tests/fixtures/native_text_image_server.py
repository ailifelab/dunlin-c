#!/usr/bin/env python3
"""Loopback text/SSE/image fixture only. Never opens native application state."""
import argparse,base64,http.server,json,pathlib,sys,threading,struct,zlib,time
p=argparse.ArgumentParser();p.add_argument('--port',type=int,required=True);p.add_argument('--report',type=pathlib.Path,required=True);p.add_argument('--image-rgb',help='Owned fixture variant, six hexadecimal RGB digits');p.add_argument('--text-delay',type=float,default=0);p.add_argument('--stream-pause',type=float,default=0,help='Pause between two visible SSE chunks in this loopback fixture');a=p.parse_args();assert 0<=a.text_delay<=25 and 0<=a.stream_pause<=25;root=pathlib.Path.cwd();report=a.report.resolve();assert report.is_relative_to(root/'reports') and not report.exists();report.mkdir(parents=True);lock=threading.Lock()
image_b64='iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII='
if a.image_rgb:
 assert len(a.image_rgb)==6
 rgb=bytes.fromhex(a.image_rgb)
 def chunk(tag,data):return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data))
 image_b64=base64.b64encode(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1,1,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0'+rgb))+chunk(b'IEND',b'')).decode()
class Handler(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  size=int(self.headers.get('Content-Length','0'))
  if not 0<size<=16777216:self.send_error(413);return
  raw=self.rfile.read(size)
  try:request=json.loads(raw)
  except Exception:self.send_error(400);return
  image=self.path.endswith('/images/generations');authorized=not image or self.headers.get('Authorization')=='Bearer fixture-image-key'
  if not authorized:self.send_error(401);return
  with lock:
   with (report/'requests.jsonl').open('a') as f:f.write(json.dumps({'path':self.path,'request':request,'fixtureImageAuth':authorized if image else None},ensure_ascii=False)+'\n')
  text='Owned React baseline completed.'
  if not image:time.sleep(a.text_delay)
  if not image and request.get('stream') and a.stream_pause:
   self.send_response(200);self.send_header('Content-Type','text/event-stream');self.end_headers()
   try:
    self.wfile.write(('data: '+json.dumps({'choices':[{'index':0,'delta':{'content':'Owned React '},'finish_reason':None}]})+'\n\n').encode());self.wfile.flush();time.sleep(a.stream_pause)
    self.wfile.write(('data: '+json.dumps({'choices':[{'index':0,'delta':{'content':'baseline completed.'},'finish_reason':None}]})+'\n\ndata: '+json.dumps({'choices':[{'index':0,'delta':{},'finish_reason':'stop'}]})+'\n\ndata: [DONE]\n\n').encode());self.wfile.flush()
   except (BrokenPipeError,ConnectionResetError):pass
   return
  if image:body=json.dumps({'created':1,'data':[{'b64_json':image_b64}]}).encode();mime='application/json'
  elif request.get('stream'):
   values=[{'choices':[{'index':0,'delta':{'role':'assistant','content':text},'finish_reason':None}]},{'choices':[{'index':0,'delta':{},'finish_reason':'stop'}],'usage':{'prompt_tokens':1,'completion_tokens':1,'total_tokens':2}}]
   body=(''.join('data: '+json.dumps(value)+'\n\n' for value in values)+'data: [DONE]\n\n').encode();mime='text/event-stream'
  else:body=json.dumps({'choices':[{'index':0,'message':{'role':'assistant','content':text},'finish_reason':'stop'}]}).encode();mime='application/json'
  self.send_response(200);self.send_header('Content-Type',mime);self.send_header('Content-Length',str(len(body)));self.end_headers()
  try:self.wfile.write(body)
  except (BrokenPipeError,ConnectionResetError):pass
class Server(http.server.ThreadingHTTPServer):allow_reuse_address=True;daemon_threads=False
server=Server(('127.0.0.1',a.port),Handler);threading.Thread(target=server.serve_forever,daemon=True).start();print('Owned text/SSE/image fixture at http://127.0.0.1:'+str(server.server_port)+'/v1; no native DB open',flush=True)
try:sys.stdin.readline()
finally:server.shutdown();server.server_close()
