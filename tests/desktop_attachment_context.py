#!/usr/bin/env python3
"""Real C compaction seam with fixed text/vision wire fixtures, no live model."""
import ctypes as C,http.server,json,pathlib,struct,sys,tempfile,threading,zlib
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),
 ('dc_attachment_stage',[P,S,S,S,S,C.c_int,P,C.c_size_t],P),('dc_run_message_with_secret',[P,S,S,S,P,S,S,C.POINTER(C.c_int)],P)]:
 fn=getattr(lib,name);fn.argtypes=args;fn.restype=result
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
def enc(v):return json.dumps(v,ensure_ascii=False).encode()
requests=[];errors=[]
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  try:
   req=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(req)
   summary=any('DUNLIN_CONTEXT_CHECKPOINT_V1' in str(m.get('content')) for m in req['messages'])
   answer='固定来源摘要。' if summary else '固定模型结果；没有证明视觉识别。'
   raw=enc(dict(choices=[dict(index=0,message=dict(role='assistant',content=answer),finish_reason='stop')]))
   self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
  except BaseException as e:errors.append(repr(e));self.close_connection=True
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
@EV
def event(*_):pass
@AP
def approve(*_):errors.append('unexpected approval');return 0
@CA
def cancel(*_):return 0
hooks=Hooks(None,event,approve,cancel)
def chunk(k,d):return struct.pack('>I',len(d))+k+d+struct.pack('>I',zlib.crc32(k+d))
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1,1,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0\x17\x2a\x63\xff'))+chunk(b'IEND',b'')
try:
 with tempfile.TemporaryDirectory(prefix='dunlin-attachment-context-') as tmp:
  root=pathlib.Path(tmp);root.chmod(0o700);data=root/'private';data.mkdir(mode=0o700);work=root/'project';work.mkdir()
  db=lib.dc_open(str(data).encode());assert db;assert lib.dc_add_chat(db,b'context',b'Context',str(work).encode())
  a=value(lib.dc_attachment_stage(db,b'context',None,'图片.png'.encode(),b'file',1,png,len(png)));assert a['ok'],a
  config=dict(base_url=f'http://127.0.0.1:{server.server_port}/v1',model='fixture',protocol='chat',stream=False,max_tokens=256,desktop_tools='noTools',desktop_input_modalities=['text','image'],desktop_context=dict(modelLimit=8192,deploymentLimit=0,selectedLimit=8192,outputLimit=4096,requestedOutput=256,reserve=128,strategy='semantic',automatic=True,threshold=.25,retainTurns=1,summaryOutput=256))
  caption='不要删除 A；只修改 B。'
  def run(prompt,attachment=None):
   flag=C.c_int();r=value(lib.dc_run_message_with_secret(db,b'context',enc(config),prompt.encode(),C.byref(hooks),b'',attachment.encode() if attachment else None,C.byref(flag)));assert r['status']=='completed' and flag.value==1,(r,flag.value)
  run(caption,a['data']['id']);run('x'*3000)
  summary=[r for r in requests if any('DUNLIN_CONTEXT_CHECKPOINT_V1' in str(m.get('content')) for m in r['messages'])];assert summary
  assert all(isinstance(m.get('content'),str) or m.get('content') is None for r in summary for m in r['messages'])
  assert all('data:image' not in json.dumps(r) and 'base64' not in json.dumps(r) for r in summary)
  derived=[m['content'] for r in requests if r not in summary for m in r['messages'] if isinstance(m.get('content'),str) and m['content'].startswith('Earlier conversation checkpoint')];assert derived
  protected=[line for text in derived for line in text.splitlines() if line.startswith('[source user message')]
  assert protected and any(caption in line for line in protected),protected
  assert not any('Image attachment reference' in line or 'Pixels are absent' in line or 'metadata alone' in line for line in protected),('attachment checkpoint guidance was mislabeled as an original user quotation',protected)
  assert not any('_dunlin_' in key for r in summary for m in r['messages'] for key in m),summary
  config['desktop_context'].update(modelLimit=65536,selectedLimit=65536,threshold=.95,omitHistoryImages=True)
  staged=value(lib.dc_attachment_stage(db,b'context',None,b'new.png',b'file',1,png,len(png)));assert staged['ok']
  run('new image',staged['data']['id'])
  inputs=requests[-1]['messages']
  assert sum(isinstance(m.get('content'),list) for m in inputs)==1 and isinstance(inputs[-1]['content'],list),inputs
  config['desktop_context']['omitHistoryImages']=False
  run('restore previous pictures')
  assert sum(isinstance(m.get('content'),list) for m in requests[-1]['messages'])==2
  print('PASS historical images omitted on request, current image still sent, both originals restored by switching preference off')
  assert not errors,errors;lib.dc_close(db)
  print('PASS semantic summary receives only text references; protected quotations use original caption/sequence, not generated attachment guidance')
finally:server.shutdown();server.server_close();worker.join(2)
