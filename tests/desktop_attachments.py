#!/usr/bin/env python3
"""Owned C attachment/source/protocol integration, no real model or approvals."""
import base64,ctypes as C,hashlib,http.server,json,pathlib,sqlite3,struct,sys,tempfile,threading,zlib
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),
 ('dc_add_chat',[P,S,S,S],C.c_int),('dc_delete_chat',[P,S],C.c_int),('dc_messages',[P,S],P),
 ('dc_workspace_request',[P,S],P),('dc_read_view',[S,S],P),('dc_index_pending',[P,C.c_uint],C.c_int),
 ('dc_attachment_stage',[P,S,S,S,S,C.c_int,P,C.c_size_t],P),
 ('dc_run_message_with_secret',[P,S,S,S,P,S,S,C.POINTER(C.c_int)],P)]:
 fn=getattr(lib,name);fn.argtypes=args;fn.restype=result
def enc(v):return json.dumps(v,ensure_ascii=False).encode()
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1,1,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0\x17\x2a\x63\xff'))+chunk(b'IEND',b'')
body='不要删除 A；只修改 B。\r\n引号 " 与 café 👩‍💻 原样。'.encode();requests=[];errors=[];fail=False
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  try:
   request=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(request)
   raw=enc(dict(choices=[dict(index=0,message=dict(role='assistant',content='固定协议结果；不是视觉识别证明。'),finish_reason='stop')]))
   self.send_response(503 if fail else 200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
  except BaseException as e:errors.append(repr(e));self.close_connection=True
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
@EV
def event(*_):pass
@AP
def approval(*_):errors.append('unexpected model tool approval');return 0
@CA
def cancelled(*_):return 0
hooks=Hooks(None,event,approval,cancelled)
try:
 with tempfile.TemporaryDirectory(prefix='dunlin-attachments-') as tmp:
  root=pathlib.Path(tmp);data=root/'private';data.mkdir(mode=0o700);work=root/'project';work.mkdir();db=lib.dc_open(str(data).encode());assert db
  def req(op,**kw):return value(lib.dc_workspace_request(db,enc(dict(op=op,**kw))))
  def ok(op,**kw):
   r=req(op,**kw);assert r['ok'],r;return r.get('data')
  def rows(chat='image'):return value(lib.dc_messages(db,chat.encode()))
  def read(op,**kw):return value(lib.dc_read_view(str(data).encode(),enc(dict(op=op,**kw))))
  def stage(chat,payload,format=1,expected=None,name='原图.png'):
   return value(lib.dc_attachment_stage(db,chat.encode(),expected.encode() if expected else None,name.encode(),b'file',format,payload,len(payload)))
  config=dict(base_url=f'http://127.0.0.1:{server.server_port}/v1',model='fixture',protocol='chat',stream=False,max_tokens=256,desktop_tools='noTools',desktop_input_modalities=['text','image'])
  def run(chat='image',prompt='相同描述',attachment=None,cfg=None):
   flag=C.c_int(99);r=value(lib.dc_run_message_with_secret(db,chat.encode(),enc(cfg or config),prompt.encode() if prompt is not None else None,C.byref(hooks),b'',attachment.encode() if attachment else None,C.byref(flag)));return r,flag.value
  def inject():
   sql=sqlite3.connect(data/'conversations.sqlite');sql.create_function('dunlin_workspace_writer_v1',0,lambda:1);sql.create_function('dunlin_attachment_writer_v1',0,lambda:1);return sql
  for chat in ['image','text','other','fault','transport','empty']:assert lib.dc_add_chat(db,chat.encode(),chat.encode(),str(work).encode())
  assert ok('attachments.pending',chat='image') is None
  a=stage('image',png)['data'];assert a['sha256']==hashlib.sha256(png).hexdigest() and a['width']==1
  assert not stage('image',png)['ok'];assert not stage('image',b'broken',expected=a['id'])['ok'];assert ok('attachments.pending',chat='image')==a
  assert not read('attachments.read',chat='other',draftID=a['id'])['ok']
  pending=read('attachments.read',chat='image',draftID=a['id']);assert pending['ok'] and base64.b64decode(pending['data']['dataURL'].split(',')[1])==png
  assert read('attachments.pending',chat='image')['data']==a
  r,committed=run();assert r['status']=='failed' and committed==0 and not requests
  r,committed=run(attachment=a['id'],cfg=dict(config,desktop_input_modalities=['text']));assert r['status']=='failed' and not committed and not requests
  assert rows()==[] and ok('attachments.pending',chat='image')==a
  r,committed=run(attachment=a['id']);assert r['status']=='completed' and committed==1,(r,committed)
  assert ok('attachments.pending',chat='image') is None
  original=rows();assert len(original)==2 and original[0]['content']=='相同描述' and original[0]['attachment']['id']==a['id']
  image=original[0]['attachment'];assert image['canonicalHash'] and image['sequence']==0
  request=requests[-1];user=[m for m in request['messages'] if m['role']=='user'][0];assert isinstance(user['content'],list)
  assert base64.b64decode(user['content'][1]['image_url']['url'].split(',')[1])==png
  assert not any('_dunlin_' in k for m in request['messages'] for k in m),request
  canonical=inject().execute("SELECT state FROM chats WHERE id='image'").fetchone()[0]
  assert 'base64' not in canonical and 'data:image' not in canonical and '_dunlin_source_seq' not in canonical
  assert read('attachments.read',chat='image',sequence=0,canonicalHash=image['canonicalHash'])['ok']
  assert not read('attachments.read',chat='other',sequence=0,canonicalHash=image['canonicalHash'])['ok']
  assert not read('attachments.read',chat='image',sequence=0,canonicalHash='0'*64)['ok']
  r,committed=run();assert r['status']=='completed' and committed==1 and rows()[:2]==original
  user=[m for m in requests[-1]['messages'] if m['role']=='user'];assert len(user)==2 and isinstance(user[0]['content'],list) and user[1]['content']=='相同描述',user
  print('PASS exact attachment admission, declared image capability, owner/source reads, original source and repeated-caption sequence routing')
  vision=dict(id='vision',name='Vision fixture',endpoint=config['base_url'],model='fixture',protocolName='chat',maxOutput=256,inputModalities=['text','image'])
  plain=dict(vision,id='plain',name='Text fixture',model='fixture-text',inputModalities=['text'])
  registry=ok('config.save',revision=0,models=[vision,plain],providers=[]);ok('chats.update',id='image',field='model',value='vision')
  assert ok('config.model',id='vision',revision=registry['revision'])['desktop_input_modalities']==['text','image']
  def preview(target='plain',policy='images'):
   return ok('chats.model.preview',chat='image',target=target,revision=registry['revision'],imagePolicy=policy)
  def apply(plan):
   return req('chats.model.apply',chat='image',target=plan['targetID'],revision=plan['configRevision'],currentID=plan['currentID'],historyHash=plan['historyHash'],pendingID=plan['pendingID'],currentImagePolicy=plan['currentImagePolicy'],imagePolicy=plan['imagePolicy'])
  blocked=preview();assert not blocked['canSwitch'] and blocked['imageCount']==1 and blocked['canTextOnly']
  with inject() as sql:before_policy=sql.execute("SELECT state FROM chats WHERE id='image'").fetchone()[0]
  text_plan=preview(policy='textOnly');assert text_plan['canSwitch'] and apply(text_plan)['ok']
  with inject() as sql:assert sql.execute("SELECT state FROM chats WHERE id='image'").fetchone()[0]==before_policy
  assert read('attachments.read',chat='image',sequence=0,canonicalHash=image['canonicalHash'])['ok']
  r,committed=run(prompt='显式仅传文字',cfg=dict(config,model='fixture-text',desktop_input_modalities=['text']));assert r['status']=='completed' and committed
  assert 'data:image' not in json.dumps(requests[-1]) and 'base64' not in json.dumps(requests[-1])
  assert rows()[:2]==original and image['id'] in json.dumps(requests[-1])
  queued=stage('image',png)['data'];pending_plan=preview(target='vision');assert pending_plan['pendingImageCount']==1 and pending_plan['pendingID']==queued['id']
  changed=stage('image',png,expected=queued['id'])['data'];assert changed['id']!=queued['id']
  assert apply(pending_plan)['error']=='model_switch_history_changed';assert ok('chats.metadata',id='image')['model']=='plain'
  pending_plan=preview(target='vision');assert apply(pending_plan)['ok']
  r,committed=run(attachment=changed['id']);assert r['status']=='completed' and committed
  image_parts=[part for m in requests[-1]['messages'] if isinstance(m.get('content'),list) for part in m['content'] if part.get('type')=='image_url']
  assert len(image_parts)==2 and all(base64.b64decode(p['image_url']['url'].split(',')[1])==png for p in image_parts)
  print('PASS explicit text-only provider projection preserves originals, declared capability mapping, pending-selection CAS and restored image mode')
  t=stage('text',body,2,name='资料.txt')['data'];r,committed=run('text','只按原文回答',t['id'],dict(config,desktop_input_modalities=['text']));assert r['status']=='completed' and committed
  user=[m for m in requests[-1]['messages'] if m['role']=='user'][0];assert isinstance(user['content'],str) and json.dumps(body.decode(),ensure_ascii=False)[1:-1] in user['content']
  text_rows=rows('text');assert text_rows[0]['content']=='只按原文回答' and text_rows[0]['attachment']['sha256']==hashlib.sha256(body).hexdigest()
  text_image=text_rows[0]['attachment'];assert read('attachments.read',chat='text',sequence=0,canonicalHash=text_image['canonicalHash'])['data']['text'].encode()==body
  empty=stage('empty',b'',2,name='empty.txt')['data'];r,committed=run('empty','',empty['id']);assert r['status']=='completed' and committed and rows('empty')[0]['content']==''
  assert read('attachments.read',chat='empty',sequence=0,canonicalHash=rows('empty')[0]['attachment']['canonicalHash'])['data']['text']==''
  with inject() as sql:
   indexed=[r[0] for r in sql.execute('SELECT text FROM index_jobs')];assert not any('base64' in x or body.decode() in x or '请查看附件。' in x for x in indexed)
  assert lib.dc_index_pending(db,128)>=0
  print('PASS exact quoted UTF8/CRLF text requests, empty-caption/empty-file source display and no attachment payload in lexical jobs')
  fault=stage('fault',body,2,name='故障.txt')['data'];before_requests=len(requests)
  ok('chats.update',id='fault',field='draft',value='保留待发')
  with inject() as sql:sql.execute("CREATE TRIGGER input_fault BEFORE INSERT ON index_jobs WHEN NEW.chat='fault' BEGIN SELECT RAISE(ABORT,'owned input commit failure');END")
  r,committed=run('fault','保留待发',fault['id']);assert r['status']=='failed' and not committed and len(requests)==before_requests and rows('fault')==[]
  assert ok('attachments.pending',chat='fault')==fault
  assert ok('chats.metadata',id='fault')['draft']=='保留待发'
  with inject() as sql:assert sql.execute("SELECT count(*) FROM attachment_messages WHERE owner='fault'").fetchone()[0]==0;sql.execute('DROP TRIGGER input_fault')
  network=stage('transport',body,2,name='网络.txt')['data'];ok('chats.update',id='transport',field='draft',value='已提交后网络故障');fail=True;r,committed=run('transport','已提交后网络故障',network['id']);fail=False
  assert r['status']=='failed' and committed==1 and ok('attachments.pending',chat='transport') is None and rows('transport')[0]['content']=='已提交后网络故障'
  assert ok('chats.metadata',id='transport')['draft']==''
  print('PASS input/blob/derived-index atomic rollback and distinct durable-input boundary after actual HTTP503')
  source=root/'中文资料.txt';source.write_bytes(body)
  reviewed=ok('attachments.previewFile',chat='other',path=str(source),format='text');assert reviewed['text'].encode()==body and ok('attachments.pending',chat='other') is None
  source.write_bytes(b'changed after review');assert not req('attachments.stageFile',chat='other',path=str(source),format='text',expectedSHA=reviewed['sha256'])['ok'];assert ok('attachments.pending',chat='other') is None
  source.write_bytes(body);file=ok('attachments.stageFile',chat='other',path=str(source),format='text',expectedSHA=reviewed['sha256']);assert file['sha256']==hashlib.sha256(body).hexdigest()
  symlink=root/'alias.txt';symlink.symlink_to(source);assert not req('attachments.stageFile',chat='other',expectedID=file['id'],path=str(symlink),format='text')['ok']
  assert not req('attachments.stageFile',chat='other',expectedID=file['id'],path=str(root),format='text')['ok']
  bad=root/'bad.png';bad.write_bytes(b'not png');assert not req('attachments.stageFile',chat='other',expectedID=file['id'],path=str(bad),format='png')['ok']
  assert ok('attachments.pending',chat='other')==file
  assert not req('attachments.remove',chat='other',expectedID=t['id'])['ok'];ok('attachments.remove',chat='other',expectedID=file['id']);assert ok('attachments.pending',chat='other') is None
  backups=list((data/'Backups').glob('before-chat-attachments-v1-*.sqlite'));assert len(backups)==1
  with sqlite3.connect(backups[0]) as copy:
   assert copy.execute('PRAGMA integrity_check').fetchone()[0]=='ok';assert copy.execute('SELECT count(*) FROM chats').fetchone()[0]==6;assert not copy.execute("SELECT 1 FROM sqlite_master WHERE name='attachment_resources'").fetchone()
  with inject() as sql:before_count=sql.execute('SELECT count(*) FROM attachment_resources').fetchone()[0]
  assert lib.dc_delete_chat(db,b'image') and not read('attachments.read',chat='image',sequence=0,canonicalHash=image['canonicalHash'])['ok']
  with inject() as sql:assert not sql.execute("SELECT 1 FROM attachment_messages WHERE scope=1 AND owner='image'").fetchone();assert sql.execute('SELECT count(*) FROM attachment_resources').fetchone()[0]==before_count-1
  lib.dc_close(db);db=lib.dc_open(str(data).encode());assert db
  assert read('attachments.read',chat='text',sequence=0,canonicalHash=text_image['canonicalHash'])['data']['text'].encode()==body
  # The cleanup estimate must count each shared blob only after its last other owner disappears.
  for identity in (b'cleanup-a',b'cleanup-b'):
   assert lib.dc_add_chat(db,identity,identity,str(work).encode())
  def exclusive(chat):return ok('chats.cleanup.preview',id=chat)['exclusiveBytes']
  before=exclusive('cleanup-a')
  assert stage('cleanup-a',png)['ok'] and exclusive('cleanup-a')==before+len(png)
  assert stage('cleanup-b',png)['ok'] and exclusive('cleanup-a')==before
  assert lib.dc_delete_chat(db,b'cleanup-b') and exclusive('cleanup-a')==before+len(png)
  assert lib.dc_delete_chat(db,b'cleanup-a') and not req('chats.cleanup.preview',id='cleanup-a')['ok']
  lib.dc_close(db);assert not errors,errors
  print('PASS bounded C file ingestion, symlink/type/format/stale rejection, actual pre-upgrade copy integrity, source-scoped GC and reopen')
finally:server.shutdown();server.server_close();worker.join(2)
