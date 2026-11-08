#!/usr/bin/env python3
"""Crash before a fixture model response, then change defaults and resume frozen inputs.
Only the owned child process is killed; no commands or external model are used.
"""
import ctypes as C,sys,json,pathlib,tempfile,http.server,threading,subprocess,time,sqlite3
libpath=str(pathlib.Path(sys.argv[1]).resolve())
lib=C.CDLL(libpath);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_run',[P,S,S,S,P],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_workspace_request',[P,S],P),('dc_messages',[P,S],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
def val(raw):
 assert raw
 try:return json.loads(C.string_at(raw))
 finally:lib.dc_free(raw)
if len(sys.argv)>2 and sys.argv[2]=='--child':
 db=lib.dc_open(sys.argv[3].encode());assert db;lib.dc_run(db,b'chat',sys.argv[4].encode(),b'Original user question',None);raise SystemExit(5)
requests=[];started=threading.Event();release=threading.Event()
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  data=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(data)
  if len(requests)==1:started.set();release.wait(20)
  body=json.dumps({'choices':[{'index':0,'message':{'role':'assistant','content':'fixture answer'},'finish_reason':'stop'}]}).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers()
  try:self.wfile.write(body)
  except (BrokenPipeError,ConnectionResetError):pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='dunlin-frozen-') as tmp:
 root=pathlib.Path(tmp);state=root/'private';state.mkdir(mode=0o700);work=root/'work';work.mkdir();skill=root/'skill';skill.mkdir();(skill/'SKILL.md').write_text('---\nname: frozen-test\nversion: 1.0.0\n---\nFrozen original Skill instruction.\n')
 db=lib.dc_open(str(state).encode());assert db;assert lib.dc_add_chat(db,b'chat',b'Chat',str(work).encode())
 def req(op,**kw):return val(lib.dc_workspace_request(db,json.dumps(dict(op=op,**kw)).encode()))
 imported=req('skills.import',path=str(skill))['data'];assert req('skills.bind',scope='global',scopeID='',source=imported['source'],package=imported['package'],version=imported['version'],mode='enable')['ok'];lib.dc_close(db)
 original={'base_url':f'http://127.0.0.1:{server.server_port}/v1','model':'original-model','protocol':'chat','stream':False,'system_prompt':'Original system instruction.','max_tokens':256,'desktop_tools':'noTools','desktop_profile_id':'profile-1','desktop_credential_revision':0}
 process=subprocess.Popen([sys.executable,__file__,libpath,'--child',str(state),json.dumps(original)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 assert started.wait(10),'owned child did not reach model request';process.kill();process.communicate(timeout=5);release.set()
 db=lib.dc_open(str(state).encode());assert db
 assert req('skills.bind',scope='global',scopeID='',source=imported['source'],package=imported['package'],version=imported['version'],mode='disable')['ok']
 updated=dict(original,system_prompt='New default must wait until next turn.',max_tokens=64)
 result=val(lib.dc_run(db,b'chat',json.dumps(updated).encode(),None,None));assert result['status']=='completed',result
 assert requests[-1]['max_tokens']==256 and any('Original system instruction.' in m.get('content','') and 'Frozen original Skill instruction.' in m.get('content','') for m in requests[-1]['messages'])
 messages=val(lib.dc_messages(db,b'chat'));assert [m['content'] for m in messages if m['role']=='user']==['Original user question'];assert messages[-1]['content']=='fixture answer'
 segments=req('trace.segments',chat='chat')['data']['items'];assert len(segments)==2 and len({s['run'] for s in segments})==1;assert {s['status'] for s in segments}=={'interrupted','completed'},segments
 sql=sqlite3.connect(state/'conversations.sqlite');rows=sql.execute('SELECT snapshot,status FROM run_records').fetchall();assert len(rows)==1 and rows[0][1]=='completed';snapshot=json.loads(rows[0][0]);assert snapshot['_public_settings']['max_tokens']==256 and snapshot['_frozen_skills'][0]['enabled'];sql.close();lib.dc_close(db)
 print('PASS owned process crash, durable original run, frozen prompt/output/Skill versions despite changed defaults, one original user message, same run ID and completed recovery')
 print('PASS interrupted and resumed trajectory segments link to the same original durable run')
server.shutdown();server.server_close()
