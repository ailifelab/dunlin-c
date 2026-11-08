#!/usr/bin/env python3
"""Owned HTTP barriers prove overlapping C worker execution, not merely queue claims."""
import ctypes as C, pathlib, json, sys, tempfile, threading, http.server, sqlite3
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for name,args,out in [('dc_open',[S],P),('dc_open_agent_worker',[P],P),('dc_close',[P],None),('dc_free',[P],None),('dc_workspace_request',[P,S],P),('dc_agent_execute',[P,S,S,S,P],P),('dc_messages',[P,S],P)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=out
def enc(x):return json.dumps(x).encode()
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
entered=[threading.Event(),threading.Event()];release=threading.Event();requests=[];lock=threading.Lock()
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  r=json.loads(self.rfile.read(int(self.headers['Content-Length'])));task=r['messages'][-1]['content'];i=0 if task.startswith('worker zero') else 1
  with lock:requests.append(r)
  entered[i].set();released=release.wait(15)
  body=enc(dict(choices=[dict(index=0,message=dict(role='assistant',content='Reply '+task if released else 'Fixture timeout'),finish_reason='stop')]))
  self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers()
  try:self.wfile.write(body)
  except BrokenPipeError:pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
try:
 with tempfile.TemporaryDirectory(prefix='dunlin-worker-') as temp:
  root=pathlib.Path(temp);data=root/'private';data.mkdir(mode=0o700);owner=lib.dc_open(str(data).encode());assert owner
  workers=[];threads=[]
  try:
   def req(op,**kw):
    r=value(lib.dc_workspace_request(owner,enc(dict(op=op,**kw))));assert r['ok'],r;return r.get('data')
   model=dict(id='a',name='Owned model',endpoint=f'http://127.0.0.1:{server.server_port}/v1',model='owned',protocolName='chat',maxOutput=256,authentication='none')
   rev=req('config.save',revision=0,models=[model],providers=[])['revision']
   for i in range(2):
    path=root/str(i);path.mkdir();req('projects.create',id='p'+str(i),name='Project '+str(i),path=str(path),instructions='',model='a',policy='ask',context=[])
    req('agents.create',id='j'+str(i),project='p'+str(i),model='a',task=['worker zero','worker one'][i],revision=rev)
   req('agents.create',id='blocked',project='p0',model='a',task='must not run',revision=rev)
   cfg=req('config.model',id='a',revision=rev);cfg.update(stream=False,desktop_tools='noTools')
   results={}
   def run(i):results[i]=value(lib.dc_agent_execute(workers[i],('j'+str(i)).encode(),enc(cfg),b'',None))
   workers.append(lib.dc_open_agent_worker(owner));assert workers[0]
   t=threading.Thread(target=run,args=(0,));threads.append(t);t.start();assert entered[0].wait(8)
   # Opening the second worker during an active request must not recover it.
   workers.append(lib.dc_open_agent_worker(owner));assert workers[1]
   jobs={j['id']:j for j in req('agents.list')};assert jobs['j0']['state']=='running'
   blocked=value(lib.dc_agent_execute(workers[1],b'blocked',enc(cfg),b'',None));assert blocked['status']=='busy'
   assert len(requests)==1
   t=threading.Thread(target=run,args=(1,));threads.append(t);t.start();assert entered[1].wait(8),'second request did not overlap first'
   # Parent connection may close, but duplicated process lease blocks recovery.
   lib.dc_close(owner);owner=None;assert not lib.dc_open(str(data).encode())
   release.set()
   for t in threads:t.join(15);assert not t.is_alive()
   assert all(r['status']=='completed' for r in results.values()),results
   for i,w in enumerate(workers):
    messages=value(lib.dc_messages(w,('agent-j'+str(i)).encode()));assert messages[-1]['content']=='Reply '+['worker zero','worker one'][i]
    assert not any(['worker one','worker zero'][i] in m['content'] for m in messages)
   with sqlite3.connect(data/'conversations.sqlite') as db:
    assert dict(db.execute('SELECT id,state FROM agent_jobs'))=={'j0':'succeeded','j1':'succeeded','blocked':'queued'}
   with sqlite3.connect(data/'trajectory.sqlite') as db:
    assert db.execute("SELECT count(*) FROM segments WHERE status='interrupted'").fetchone()[0]==0
   for w in workers:lib.dc_close(w)
   workers=[];owner=lib.dc_open(str(data).encode());assert owner
   entered=[threading.Event(),threading.Event()];release.clear()
   for i in range(2):req('agents.create',id='cancel'+str(i),project='p'+str(i),model='a',task=['worker zero cancel','worker one continue'][i],revision=rev)
   stopped=threading.Event();CF=C.CFUNCTYPE(C.c_int,P)
   @CF
   def cancelled(_):return int(stopped.is_set())
   class Hooks(C.Structure):_fields_=[('user',P),('event',P),('approve',P),('cancelled',CF)]
   hooks=Hooks(None,None,None,cancelled);results={};threads=[]
   def cancellable(i):results[i]=value(lib.dc_agent_execute(workers[i],('cancel'+str(i)).encode(),enc(cfg),b'',C.byref(hooks) if i==0 else None))
   for i in range(2):
    workers.append(lib.dc_open_agent_worker(owner));assert workers[-1]
    t=threading.Thread(target=cancellable,args=(i,));threads.append(t);t.start()
   assert all(e.wait(8) for e in entered)
   stopped.set();threads[0].join(8);assert not threads[0].is_alive()
   assert results[0]['status']=='cancelled',results
   assert threads[1].is_alive(),'cancelling one worker affected the other'
   release.set();threads[1].join(8);assert results[1]['status']=='completed',results
   states={j['id']:j['state'] for j in req('agents.list')};assert states['cancel0']=='cancelled' and states['cancel1']=='succeeded',states
   pending=req('agents.pending');assert [j['id'] for j in pending]==['blocked']
   for w in workers:lib.dc_close(w)
   workers=[];threads=[];results={};release.clear();entered=[threading.Event(),threading.Event()]
   for i in range(2):req('agents.create',id='control'+str(i),project='p'+str(i),model='a',task=['worker zero control','worker one unaffected'][i],revision=rev)
   def controlled(i):results[i]=value(lib.dc_agent_execute(workers[i],('control'+str(i)).encode(),enc(cfg),b'',None))
   for i in range(2):
    workers.append(lib.dc_open_agent_worker(owner));assert workers[-1]
    t=threading.Thread(target=controlled,args=(i,));threads.append(t);t.start()
   assert all(e.wait(8) for e in entered)
   req('agents.cancel',id='control0')
   threads[0].join(4);assert not threads[0].is_alive(),'persisted cancellation did not stop in-flight model request'
   assert results[0]['status']=='cancelled',results
   assert threads[1].is_alive(),'control cancellation affected another worker'
   release.set();threads[1].join(8);assert results[1]['status']=='completed',results


  finally:
   release.set()
   for t in threads:t.join(20)
   for w in workers:lib.dc_close(w)
   if owner:lib.dc_close(owner)
finally:server.shutdown();server.server_close()
print('PASS overlapping actual C model requests on independent workspace workers; writer exclusion; isolated histories; no recovery on worker attach; process lease survives parent close; no interrupted trace')
