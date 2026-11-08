#!/usr/bin/env python3
"""Actual C request projection with owned fixture model, no live credentials."""
import ctypes as C,json,sys,tempfile,pathlib,threading,http.server
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_bind_identity',[P,S],C.c_int),('dc_knowledge_call',[P,S],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_messages',[P,S],P),('dc_run',[P,S,S,S,P],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
def val(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
requests=[]
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*a):pass
 def do_POST(self):
  q=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(q);body=json.dumps({'choices':[{'index':0,'message':{'role':'assistant','content':'fixture answer'},'finish_reason':'stop'}]}).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='dunlin-rag-chat-') as tmp:
 root=pathlib.Path(tmp);state=root/'state';state.mkdir(mode=0o700);work=root/'work';work.mkdir();db=lib.dc_open(str(state).encode());assert db;assert lib.dc_bind_identity(db,b'alice');assert lib.dc_add_chat(db,b'chat',b'Chat',str(work).encode())
 def op(operation,**kw):
  r=val(lib.dc_knowledge_call(db,json.dumps(dict(op=operation,**kw)).encode()));assert r['ok'],r;return r.get('data')
 m=op('memory.create',id='m1',text='NEVER auto-upgrade Node.',subject='alice',kind='preference',scope='global',scopeID='');m=op('memory.update',id=m['id'],revision=m['revision'],status='confirmed');op('knowledge.add',id='doc1',name='SQLite Manual',text='SQLite graph relations use indexed edges and atomic transactions.',scope='global',scopeID='')
 cfg={'base_url':f'http://127.0.0.1:{server.server_port}/v1','model':'A','protocol':'chat','stream':False,'max_tokens':128,'desktop_tools':'noTools','desktop_knowledge':{'memory':True,'rag':True}}
 r=val(lib.dc_run(db,b'chat',json.dumps(cfg).encode(),b'Explain SQLite',None));assert r['status']=='completed',r;serialized=json.dumps(requests[-1]);assert 'NEVER auto-upgrade Node.' in serialized and 'indexed edges' in serialized and 'subject' in serialized
 messages=val(lib.dc_messages(db,b'chat'));assert messages[0]['content']=='Explain SQLite' and len(messages)==2;assert messages[0]['sequence']==0 and len(messages[0]['sourceHash'])==64
 op('memory.remove',id='m1',revision=m['revision']);cfg['model']='B';r=val(lib.dc_run(db,b'chat',json.dumps(cfg).encode(),b'Continue without recalled memory',None));assert r['status']=='completed',r;assert 'NEVER auto-upgrade Node.' not in json.dumps(requests[-1]);assert any(m.get('content')=='Explain SQLite' for m in requests[-1]['messages'])
 lib.dc_close(db)
 print('PASS confirmed memory and local RAG reach actual request view with provenance; canonical user text/IDs unchanged; removal stops later recall across model switch')
server.shutdown();server.server_close()
