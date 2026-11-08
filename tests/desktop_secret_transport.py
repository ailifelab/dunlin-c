#!/usr/bin/env python3
"""Host-resolved credential transport, protocol headers and simultaneous stores.
All credential strings here are public disposable protocol fixtures, not user keys.
"""
import ctypes as C,json,pathlib,tempfile,sys,threading,http.server,os,concurrent.futures,sqlite3
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_run_with_secret',[P,S,S,S,P,S],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
observed=[];lock=threading.Lock()
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  data=json.loads(self.rfile.read(int(self.headers['Content-Length'])));headers=dict(self.headers)
  with lock:observed.append((self.path,data,headers))
  if self.path.endswith('/responses'):body={'id':'fixture','object':'response','output':[{'type':'message','id':'m1','status':'completed','role':'assistant','content':[{'type':'output_text','text':'done','annotations':[]}]}],'status':'completed'}
  elif self.path.endswith('/messages'):body={'id':'fixture','type':'message','role':'assistant','model':'fixture','content':[{'type':'text','text':'done'}],'stop_reason':'end_turn','usage':{'input_tokens':1,'output_tokens':1}}
  elif ':generateContent' in self.path:body={'candidates':[{'content':{'role':'model','parts':[{'text':'done'}]},'finishReason':'STOP'}]}
  else:body={'choices':[{'index':0,'message':{'role':'assistant','content':'done'},'finish_reason':'stop'}]}
  b=json.dumps(body).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(b)));self.end_headers();self.wfile.write(b)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
before=os.environ.get('DUNLIN_DESKTOP_KEY')
with tempfile.TemporaryDirectory(prefix='dunlin-secret-transport-') as td:
 def invoke(i,protocol,auth='auto'):
  path=pathlib.Path(td)/str(i);path.mkdir(mode=0o700);db=lib.dc_open(str(path).encode());assert db
  work=pathlib.Path(td)/('work-'+str(i));work.mkdir();assert lib.dc_add_chat(db,b'chat',b'chat',str(work).encode())
  cfg=json.dumps({'base_url':f'http://127.0.0.1:{server.server_port}/v1','model':f'fixture-{i}','protocol':protocol,'stream':False,'max_tokens':64,'desktop_tools':'noTools','desktop_auth':auth,'key_env':'MUST_NOT_BE_READ_FIXTURE'}).encode()
  result=value(lib.dc_run_with_secret(db,b'chat',cfg,f'request-{i}'.encode(),None,f'public-fixture-{i}'.encode()));assert result['status']=='completed',(protocol,result)
  sql=sqlite3.connect(path/'conversations.sqlite');snapshot=sql.execute('SELECT snapshot FROM run_records').fetchone()[0];sql.close()
  assert f'public-fixture-{i}' not in snapshot and 'MUST_NOT_BE_READ_FIXTURE' not in snapshot
  lib.dc_close(db)
 with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(lambda i:invoke(i,'chat'),range(4)))
 for _,body,headers in observed:
  i=body['model'].split('-')[-1];assert headers['Authorization']==f'Bearer public-fixture-{i}'
 print('PASS simultaneous C stores send their own host-resolved key; no process-env credential exchange')
 for i,protocol,auth in [(4,'responses','auto'),(5,'anthropic','auto'),(6,'gemini','auto'),(7,'anthropic','bearer')]:invoke(i,protocol,auth)
 for path,body,headers in observed[4:]:
  if ':generateContent' in path:assert headers.get('x-goog-api-key')=='public-fixture-6' and 'Authorization' not in headers
  elif path.endswith('/messages') and body['model']=='fixture-5':assert headers.get('x-api-key')=='public-fixture-5' and 'Authorization' not in headers
  elif body.get('model')=='fixture-7':assert headers['Authorization']=='Bearer public-fixture-7'
 print('PASS Chat/Responses bearer, Anthropic API-key or explicit bearer, Gemini API-key protocol headers')
 assert os.environ.get('DUNLIN_DESKTOP_KEY')==before
 print('PASS snapshots contain no resolved credential values; no key_env lookup in host-key mode')
server.shutdown();server.server_close()
