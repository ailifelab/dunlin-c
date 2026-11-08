#!/usr/bin/env python3
"""C provider inventory: protocol auth, advertised capabilities and redirect denial."""
import ctypes as C,json,http.server,threading,sys
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p;lib.dc_probe_model.argtypes=[S,S,P];lib.dc_probe_model.restype=P;lib.dc_free.argtypes=[P]
observed=[]
class Service(http.server.BaseHTTPRequestHandler):
 def log_message(self,*a):pass
 def do_GET(self):
  observed.append((self.path,dict(self.headers)))
  if self.path.startswith('/redirect'):
   self.send_response(302);self.send_header('Location',f'http://127.0.0.1:{self.server.server_port}/sink/models');self.end_headers();return
  status=401 if self.path.startswith('/deny') else 200
  body={'data':[{'id':'fixture-model','context_window':8192,'max_output_tokens':512}]}
  if self.path.startswith('/gemini'):body={'models':[{'name':'models/fixture-model','inputTokenLimit':4096,'outputTokenLimit':256}]}
  if self.path.startswith('/ollama'):body={'models':[{'name':'fixture-model'}]}
  if self.path.startswith('/deny'):body={'error':'public-fixture-key'}
  raw=(b'{"data":[],"data":[{"id":"fixture-model","context_window":999}]}' if self.path.startswith('/duplicate') else json.dumps(body).encode())
  if self.path.startswith('/oversize'):raw=b'x'*1048577
  self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers()
  try:self.wfile.write(raw)
  except (BrokenPipeError,ConnectionResetError):pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Service);threading.Thread(target=server.serve_forever,daemon=True).start()
def probe(route,protocol='chat',model='fixture-model',key='public-fixture-key',auth='auto'):
 cfg={'base_url':f'http://127.0.0.1:{server.server_port}'+route,'model':model,'protocol':protocol,'desktop_auth':auth};p=lib.dc_probe_model(json.dumps(cfg).encode(),key.encode(),None);assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
r=probe('/chat');assert r['ok'] and r['modelFound'] and r['contextLimit']==8192 and r['outputLimit']==512;assert observed[-1][1]['Authorization']=='Bearer public-fixture-key'
r=probe('/anthropic','anthropic');assert r['modelFound'];assert observed[-1][1]['x-api-key']=='public-fixture-key' and 'Authorization' not in observed[-1][1]
r=probe('/gemini','gemini');assert r['contextLimit']==4096 and r['source']=='provider-input-limit-conservative';assert observed[-1][1]['x-goog-api-key']=='public-fixture-key'
r=probe('/ollama/api','ollama');assert r['modelFound'] and r['contextLimit']==0;assert observed[-1][0]=='/ollama/api/tags'
print('PASS provider auth/route mapping and explicit capability metadata; unknown limits stay unknown')
assert not probe('/chat',model='unknown')['modelFound'];r=probe('/deny');assert not r['ok'] and r['httpStatus']==401 and 'public-fixture-key' not in json.dumps(r)
n=len(observed);r=probe('/redirect');assert not r['ok'] and r['httpStatus']==302 and len(observed)==n+1 and not any('/sink/' in p for p,_ in observed)
r=probe('/duplicate');assert r['ok'] and not r['listKnown'] and not r['modelFound'];assert not probe('/oversize')['ok']
n=len(observed);assert not probe('/chat',key='bad\nkey')['ok'] and len(observed)==n
print('PASS redirected credentials not forwarded; duplicate JSON/oversize/malformed key fail without fabricated capability')
server.shutdown();server.server_close()
