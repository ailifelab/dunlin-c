#!/usr/bin/env python3
"""Actual old/new C processes, owned SQLite copy and interrupted media input.

No credentials, model engine, real approvals or user directories. Normal only;
the killed Python writer/system libraries are not a sanitizer coverage claim.
"""
import ctypes as C,http.server,json,pathlib,shutil,sqlite3,struct,subprocess,sys,tempfile,threading,zlib
P=C.c_void_p;S=C.c_char_p
def load(path,current):
 lib=C.CDLL(str(pathlib.Path(path).resolve()))
 entries=[('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_run_with_secret',[P,S,S,S,P,S],P)]
 if current:entries += [('dc_attachment_stage',[P,S,S,S,S,C.c_int,P,C.c_size_t],P),('dc_run_message_with_secret',[P,S,S,S,P,S,S,C.POINTER(C.c_int)],P)]
 for name,args,result in entries:f=getattr(lib,name);f.argtypes=args;f.restype=result
 return lib
def enc(v):return json.dumps(v,ensure_ascii=False).encode()
def val(lib,p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
def chunk(k,d):return struct.pack('>I',len(d))+k+d+struct.pack('>I',zlib.crc32(k+d))
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1,1,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0\x17\x2a\x63\xff'))+chunk(b'IEND',b'')
def config(url,media=False,interrupted=False):
 out=dict(base_url=url,model='interrupted-fixture' if interrupted else 'fixture',protocol='chat',stream=False,max_tokens=256,desktop_tools='noTools')
 if media:out['desktop_input_modalities']=['text','image']
 return out
if sys.argv[1]=='--old-child':
 lib=load(sys.argv[2],False);db=lib.dc_open(sys.argv[3].encode());assert db
 prompt=None if sys.argv[5]=='resume' else sys.argv[5].encode()
 r=val(lib,lib.dc_run_with_secret(db,b'media',enc(config(sys.argv[4],interrupted=sys.argv[5]=='resume')),prompt,None,b''));lib.dc_close(db);print(json.dumps(r));raise SystemExit(0)
if sys.argv[1]=='--new-child':
 lib=load(sys.argv[2],True);db=lib.dc_open(sys.argv[3].encode());assert db
 assert lib.dc_add_chat(db,b'media',b'Interrupted media',sys.argv[4].encode())
 a=val(lib,lib.dc_attachment_stage(db,b'media',None,b'picture.png',b'file',1,png,len(png)));assert a['ok']
 flag=C.c_int();val(lib,lib.dc_run_message_with_secret(db,b'media',enc(config(sys.argv[5],True,True)),b'interrupted input',None,b'',a['data']['id'].encode(),C.byref(flag)))
 lib.dc_close(db);raise SystemExit('Writer should have been killed while HTTP response was blocked')
current=pathlib.Path(sys.argv[1]).resolve();old=pathlib.Path(sys.argv[2]).resolve();lib=load(current,True)
requests=[];errors=[];blocked=threading.Event();release=threading.Event();block_once=True
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  global block_once
  try:
   req=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(req)
   if req['model']=='interrupted-fixture' and block_once:
    block_once=False;blocked.set();assert release.wait(15)
   raw=enc(dict(choices=[dict(index=0,message=dict(role='assistant',content='fixed local protocol result'),finish_reason='stop')]))
   self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
  except (BrokenPipeError,ConnectionResetError):pass
  except BaseException as e:errors.append(repr(e));self.close_connection=True
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start();url=f'http://127.0.0.1:{server.server_port}/v1'
def child(*args):
 result=subprocess.run([sys.executable,'-B',str(pathlib.Path(__file__).resolve()),*map(str,args)],capture_output=True,text=True,timeout=15)
 assert result.returncode==0,(result.returncode,result.stdout,result.stderr);return json.loads(result.stdout)
def source(data):
 with sqlite3.connect(data/'conversations.sqlite') as sql:return sql.execute("SELECT state FROM chats WHERE id='media'").fetchone()[0]
try:
 with tempfile.TemporaryDirectory(prefix='dunlin-media-downgrade-') as tmp:
  root=pathlib.Path(tmp);data=root/'private';data.mkdir(mode=0o700);work=root/'project';work.mkdir();db=lib.dc_open(str(data).encode());assert db
  assert lib.dc_add_chat(db,b'media',b'Media writer',str(work).encode())
  a=val(lib,lib.dc_attachment_stage(db,b'media',None,b'picture.png',b'file',1,png,len(png)));assert a['ok']
  flag=C.c_int();result=val(lib,lib.dc_run_message_with_secret(db,b'media',enc(config(url,True)),b'owned source',None,b'',a['data']['id'].encode(),C.byref(flag)));assert result['status']=='completed' and flag.value==1
  pending=val(lib,lib.dc_attachment_stage(db,b'media',None,b'pending.png',b'file',1,png,len(png)));assert pending['ok'];lib.dc_close(db)
  original=source(data);count=len(requests);result=child('--old-child',old,data,url,'old input must not be admitted')
  assert result['status']=='failed' and len(requests)==count and source(data)==original,result
  with sqlite3.connect(data/'conversations.sqlite') as sql:assert sql.execute("SELECT id FROM attachment_drafts WHERE scope=1 AND owner='media'").fetchone()[0]==pending['data']['id'];assert sql.execute('SELECT count(*) FROM attachment_messages').fetchone()[0]==1
  backups=list((data/'Backups').glob('before-chat-attachments-v1-*.sqlite'));assert len(backups)==1
  restore=root/'restored';restore.mkdir(mode=0o700);shutil.copy2(backups[0],restore/'conversations.sqlite')
  with sqlite3.connect(restore/'conversations.sqlite') as sql:assert sql.execute('PRAGMA integrity_check').fetchone()[0]=='ok';assert not sql.execute("SELECT 1 FROM sqlite_master WHERE name='attachment_resources'").fetchone()
  result=child('--old-child',old,restore,url,'restored copy input');assert result['status']=='completed' and len(requests)==count+1
  assert source(data)==original
  print('PASS actual pre-media C writer rejected before HTTP/source mutation; independent pre-upgrade SQLite copy restores old-writer operation')
  interrupted=root/'interrupted';interrupted.mkdir(mode=0o700)
  process=subprocess.Popen([sys.executable,'-B',str(pathlib.Path(__file__).resolve()),'--new-child',str(current),str(interrupted),str(work),url],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  try:
   assert blocked.wait(10),'owned new writer never reached HTTP';active=source(interrupted);checkpoint=json.loads(active)
   assert checkpoint['active'] and len(checkpoint['messages'])==1 and checkpoint['messages'][0]['_dunlin_attachments']
   process.kill();stdout,stderr=process.communicate(timeout=5);assert process.returncode!=0
  finally:
   if process.poll() is None:process.kill();process.communicate(timeout=5)
   release.set()
  count=len(requests);result=child('--old-child',old,interrupted,url,'resume')
  assert result['status'] in ['failed','busy'] and len(requests)==count and source(interrupted)==active,result
  db=lib.dc_open(str(interrupted).encode());assert db;flag=C.c_int(99)
  result=val(lib,lib.dc_run_message_with_secret(db,b'media',enc(config(url,True,True)),None,None,b'',None,C.byref(flag)))
  assert result['status']=='completed' and flag.value==0,result
  checkpoint=json.loads(source(interrupted));assert not checkpoint['active'] and len([m for m in checkpoint['messages'] if m['role']=='user'])==1 and len(requests)==count+1
  with sqlite3.connect(interrupted/'conversations.sqlite') as sql:assert sql.execute('SELECT count(*) FROM attachment_messages').fetchone()[0]==1 and sql.execute('SELECT count(*) FROM attachment_drafts').fetchone()[0]==0
  lib.dc_close(db);assert not errors,errors
  print('PASS actual killed current media writer: old image cannot resume/replay, current image resumes frozen PNG and appends no duplicate input')
finally:release.set();server.shutdown();server.server_close();worker.join(2)
