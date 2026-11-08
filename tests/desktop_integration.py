#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Own loopback model, isolated state, real C Loop/SQLite/tool effects. No live credentials."""
import desktop_fixture_sqlite as fixture_sqlite
import ctypes as C, tempfile, pathlib, json, threading, http.server, sys, sqlite3, time
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for name,args,ret in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_delete_chat',[P,S],C.c_int),('dc_chats',[P],P),('dc_messages',[P,S],P),('dc_run',[P,S,S,S,P],P),('dc_workspace_request',[P,S],P),('dc_index_pending',[P,C.c_uint],C.c_int),('dc_search',[P,S],P)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=ret
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
requests=[]; approvals=[]; allow=True; approval_delay=0; grant_boundary=[]; active_directory=None
@EV
def event(ctx,name,text):
 if name in [b'tool.post_execute',b'tool.outcome_unknown'] and active_directory:
  sql=fixture_sqlite.connect(active_directory/'conversations.sqlite')
  grant_boundary.append(sql.execute("SELECT count(*) FROM grant_records WHERE status='issued_once'").fetchone()[0]);sql.close()
@AP
def approve(ctx,name,args):
 approvals.append((name.decode(),json.loads(args)))
 if approval_delay:time.sleep(approval_delay)
 return int(allow)
@CA
def cancel(ctx):return 0
hooks=Hooks(None,event,approve,cancel)
class Handler(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  req=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(req)
  if self.path.endswith('/responses'):
   body=json.dumps({'id':'response_fixture','object':'response','status':'completed','output':[{'type':'reasoning','id':'r1','summary':[],'encrypted_content':'opaque_fixture'},{'type':'message','id':'m1','role':'assistant','status':'completed','content':[{'type':'output_text','text':'原协议回答','annotations':[]}]}]}).encode()
   self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body);return
  m=req['messages'];last=m[-1]
  if last['role']=='user' and last['content'] in ['write','command','escape']:
   action=last['content'];tool='write_file' if action!='command' else 'shell';args={'path':'../escape.txt' if action=='escape' else 'note.txt','text':'approved effect'} if tool=='write_file' else {'command':'printf command-effect > command.txt'}
   msg={'role':'assistant','content':None,'tool_calls':[{'id':'call_'+str(len(requests)),'type':'function','function':{'name':tool,'arguments':json.dumps(args)}}]};finish='tool_calls'
  else:msg={'role':'assistant','content':'原话：不要自动升级 Node；苹果。'};finish='stop'
  body=json.dumps({'id':'fixture','object':'chat.completion','choices':[{'index':0,'message':msg,'finish_reason':finish}]}).encode()
  self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
def val(ptr):
 assert ptr
 try:return json.loads(C.string_at(ptr))
 finally:lib.dc_free(ptr)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='dunlin-desktop-test-') as td:
 d=pathlib.Path(td);active_directory=d;d.chmod(0o700);work=d/'work';work.mkdir();db=lib.dc_open(str(d).encode());assert db;assert not lib.dc_open(str(d).encode()),'second owner must fail'
 def add(id):assert lib.dc_add_chat(db,id.encode(),id.encode(),str(work).encode())
 def run(id,text,model='A',protocol='chat',task_timeout=120000):
  cfg=json.dumps({'base_url':f'http://127.0.0.1:{server.server_port}/v1','model':model,'protocol':protocol,'stream':False,'task_timeout_ms':task_timeout}).encode()
  return val(lib.dc_run(db,id.encode(),cfg,text.encode() if text is not None else None,C.byref(hooks)))
 add('cross');assert run('cross','跨协议原话','R','responses')['status']=='completed'
 assert run('cross','换聊天协议','B')['status']=='completed'
 assert any(x.get('content')=='跨协议原话' for x in requests[-1]['messages'])
 assert run('cross','返回原协议','R','responses')['status']=='completed'
 assert any(x.get('encrypted_content')=='opaque_fixture' for x in requests[-1]['input'])
 sql=fixture_sqlite.connect(d/'conversations.sqlite');stored=sql.execute("SELECT state FROM chats WHERE id='cross'").fetchone()[0];sql.close();assert 'opaque_fixture' in stored
 assert 'opaque_fixture' not in json.dumps(val(lib.dc_messages(db,b'cross')))
 traces=val(lib.dc_workspace_request(db,b'{"op":"trace.segments","chat":"cross"}'))['data']['items']
 for trace in traces:
  query=json.dumps({'op':'trace.export','chat':'cross','segment':trace['id']}).encode();out=val(lib.dc_workspace_request(db,query));assert out['ok'] and 'opaque_fixture' not in json.dumps(out) and '_dunlin_provider' not in json.dumps(out)
 print('PASS Responses/Chat/Responses context continuity; opaque metadata retained privately')
 add('approval-wait');approval_delay=0.75;assert run('approval-wait','write',task_timeout=500)['status']=='completed';approval_delay=0;assert (work/'note.txt').read_text()=='approved effect';print('PASS human approval wait excluded from task budget')
 add('chat');assert run('chat','不要自动升级 Node')['status']=='completed';assert run('chat','记得吗','B')['status']=='completed'
 assert any(x.get('content')=='不要自动升级 Node' for x in requests[-1]['messages']);assert run('chat','再切回来','A')['status']=='completed'
 assert any(x.get('content')=='记得吗' for x in requests[-1]['messages']);print('PASS model A/B/A retains the same complete conversation')
 assert run('chat','write')['status']=='completed';assert (work/'note.txt').read_text()=='approved effect';assert approvals[-1][0]=='write_file';print('PASS actual file write after explicit callback')
 assert run('chat','command')['status']=='completed';assert (work/'command.txt').read_text()=='command-effect';print('PASS actual shell command effect after explicit callback')
 allow=False;add('deny');(work/'note.txt').unlink();r=run('deny','write');assert not (work/'note.txt').exists();print('PASS permission denial prevents file effect',r['status']);allow=True
 add('escape');r=run('escape','escape');assert not (d/'escape.txt').exists();print('PASS path escape rejected',r['status'])
 while lib.dc_index_pending(db,16)>0:pass
 hits=val(lib.dc_search(db,'苹果'.encode()));assert any(x['chat']=='chat' for x in hits);assert all('不要自动升级' in x['text'] for x in hits);print('PASS Chinese two-character retrieval preserves original negation')
 assert lib.dc_delete_chat(db,b'chat');assert all(x['chat']!='chat' for x in val(lib.dc_search(db,'苹果'.encode())));print('PASS deletion removes searchable conversation')
 add('pending');assert run('pending','first')['status']=='completed'
 sql=fixture_sqlite.connect(d/'conversations.sqlite');state=json.loads(sql.execute("SELECT state FROM chats WHERE id='pending'").fetchone()[0]);state['active']=True;sql.execute("UPDATE chats SET state=? WHERE id='pending'",(json.dumps(state),));sql.commit();sql.close();n=len(requests);assert run('pending','switch','B')['status']=='busy';assert len(requests)==n;print('PASS unfinished turn blocks model switch without model/tool call')
 lib.dc_close(db);db=lib.dc_open(str(d).encode());assert db;assert any(x['id']=='pending' for x in val(lib.dc_chats(db)));lib.dc_close(db);print('PASS restart preserves committed inventory')
assert grant_boundary and not any(grant_boundary),('single-call grants still active after tool return',grant_boundary)
print('PASS single-call grant expires at tool return before subsequent model work')
server.shutdown();server.server_close()
