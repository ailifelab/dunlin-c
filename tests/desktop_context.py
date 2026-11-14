#!/usr/bin/env python3
"""Real C request projector + summary protocol fixture; canonical history remains whole.
This checks projection/cache/budget contracts, not real-model summary fidelity.
"""
import ctypes as C,json,pathlib,tempfile,sys,sqlite3,threading,http.server
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_workspace_request',[P,S],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_messages',[P,S],P),('dc_run',[P,S,S,S,P],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
requests=[];summaries=[]
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*a):pass
 def do_POST(self):
  req=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(req);last=req['messages'][-1]['content']
  if last=='read fixture' and req['messages'][-1]['role']=='user':
   choice={'role':'assistant','content':None,'tool_calls':[{'id':'read-only','type':'function','function':{'name':'read_file','arguments':json.dumps({'path':'clip.txt'})}}]}
   body=json.dumps({'choices':[{'index':0,'message':choice,'finish_reason':'tool_calls'}]}).encode();self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body);return
  if last.startswith('DUNLIN_CONTEXT_CHECKPOINT_V1:'):summaries.append(req);answer='Confirmed constraints: NEVER auto-upgrade Node. Preserve sentinel.c. Earlier verified tool outcomes only; pending work remains pending.'
  else:answer='fixture response from '+req['model']
  body=json.dumps({'choices':[{'index':0,'message':{'role':'assistant','content':answer},'finish_reason':'stop'}]}).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
def val(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
with tempfile.TemporaryDirectory(prefix='dunlin-context-') as td:
 root=pathlib.Path(td);private=root/'state';private.mkdir(mode=0o700);work=root/'work';work.mkdir();db=lib.dc_open(str(private).encode());assert db
 assert lib.dc_add_chat(db,b'chat',b'context test',str(work).encode())
 policy={'modelLimit':4096,'deploymentLimit':0,'selectedLimit':4096,'outputLimit':512,'requestedOutput':128,'reserve':128,'strategy':'semantic','automatic':True,'threshold':.6,'retainTurns':2,'summaryOutput':128}
 def run(text,model='A',strategy='semantic',automatic=True):
  context=dict(policy,strategy=strategy,automatic=automatic)
  cfg=json.dumps({'base_url':f'http://127.0.0.1:{server.server_port}/v1','model':model,'protocol':'chat','stream':False,'max_tokens':128,'desktop_tools':'noTools','desktop_context':context}).encode()
  return val(lib.dc_run(db,b'chat',cfg,text.encode(),None))
 assert lib.dc_add_chat(db,b'clip',b'clip',str(work).encode())
 content='head '+('字'*2200)+' end';(work/'clip.txt').write_text(content)
 clip_policy=dict(policy,modelLimit=65536,selectedLimit=65536,shortenToolOutput=True)
 clip_cfg=dict(base_url=f'http://127.0.0.1:{server.server_port}/v1',model='clip',protocol='chat',stream=False,max_tokens=128,desktop_context=clip_policy)
 def clip(prompt):
  out=val(lib.dc_run(db,b'clip',json.dumps(clip_cfg).encode(),prompt.encode(),None));assert out['status']=='completed',out
  return next(m['content'] for m in requests[-1]['messages'] if m['role']=='tool')
 assert content in clip('read fixture')
 clipped=clip('inspect past result');assert content not in clipped and 'Tool output shortened' in clipped and 'head ' in clipped and ' end' in clipped
 clip_policy['shortenToolOutput']=False
 assert content in clip('restore full result')
 print('PASS completed tool output shortened only in request, UTF-8 ends intact, current tool chain exact and original restored')
 originals=[]
 for i in range(10):
  text=f'User turn {i}: NEVER auto-upgrade Node; keep sentinel.c. '+('bounded-fixture-text '*30);originals.append(text);r=run(text);assert r['status']=='completed',r
 assert summaries,'threshold should prepare derived checkpoint'
 assert any(any(m.get('content','').startswith('Prior derived checkpoint;') for m in req['messages']) for req in summaries),'later summary should reuse validated earlier checkpoint'
 messages=val(lib.dc_messages(db,b'chat'));assert len(messages)==20;assert [m['content'] for m in messages if m['role']=='user']==originals;assert not any('DUNLIN_CONTEXT_CHECKPOINT_V1' in m['content'] for m in messages)
 for req in requests:
  assert all('_dunlin_profile' not in m and '_dunlin_display' not in m for m in req['messages'])
 sql=sqlite3.connect(private/'conversations.sqlite');rows=sql.execute("SELECT source_count,before_bytes,after_bytes,measure_kind FROM compaction_records WHERE status='committed'").fetchall();assert rows and all(r[1]>r[2] and r[3]=='utf8-byte-estimate' for r in rows)
 print('PASS bounded model-backed summary view; incremental source-validated cache; original user/assistant messages retained')
 n=len(summaries);prior=len(messages);r=run('switch to B under strict budget','B');assert r['status']=='budget' and '摘要输入超过' in r['text'];assert len(val(lib.dc_messages(db,b'chat')))>=prior
 r=run('continue B with explicit recent policy','B','recent');assert r['status']=='completed',r;assert len(summaries)==n
 assert any('Earlier complete turns were excluded' in m.get('content','') for m in requests[-1]['messages'])
 print('PASS smaller target cannot silently clear history; explicit local recent policy uses zero summary calls')
 before=len(requests);r=run('x'*5000,'B','recent');assert r['status']!='completed';assert len(requests)==before
 print('PASS oversized current turn is not split or silently omitted')
 # Invalid explicit limits fail before any network call and preserve canonical text.
 bad=dict(policy,selectedLimit=99999);cfg=json.dumps({'base_url':f'http://127.0.0.1:{server.server_port}/v1','model':'A','protocol':'chat','desktop_tools':'noTools','desktop_context':bad}).encode();before=len(requests)
 r=val(lib.dc_run(db,b'chat',cfg,b'invalid window',None));assert r['status']=='failed' and len(requests)==before
 bad=dict(policy,modelLimit=-1);cfg=json.dumps({'base_url':f'http://127.0.0.1:{server.server_port}/v1','model':'A','protocol':'chat','desktop_tools':'noTools','desktop_context':bad}).encode();assert val(lib.dc_run(db,b'chat',cfg,b'negative capability',None))['status']=='failed'
 sql.close();lib.dc_close(db);db=lib.dc_open(str(private).encode());assert db
 rec=val(lib.dc_workspace_request(db,b'{"op":"context.records","chat":"chat"}'));assert rec['ok'] and rec['data']
 row=next(r for r in rec['data'] if r['status']=='committed')
 def record(chat):
  return val(lib.dc_workspace_request(db,json.dumps({'op':'context.record','chat':chat,'id':row['id']}).encode()))
 assert record('chat')['data'] and not record('another-chat')['ok']

 segments=val(lib.dc_workspace_request(db,b'{"op":"trace.segments","chat":"chat"}'))['data']['items'];observed=[]
 for segment in segments:
  query=json.dumps({'op':'trace.export','chat':'chat','segment':segment['id']}).encode();observed.extend(val(lib.dc_workspace_request(db,query))['data']['items'])
 assert sum(e['kind']=='context.model.request' for e in observed)==len(summaries)
 assert any(e['kind']=='context.model.metrics' for e in observed) and any(e['kind']=='context.model.end' and e['body']['accepted'] for e in observed)
 lib.dc_close(db)
 print('PASS invalid bounds/negative capability reject; derived records survive restart')
 print('PASS each actual summary model call is recorded as an auxiliary attempt with metrics and outcome')
server.shutdown();server.server_close()
