"""Real upstream workspace capture/rewind via native C confirmation, no LLM tools.
A deterministic text-only model turn supplies the actual native checkpoint cursor.
"""
import ctypes as C,json,pathlib,tempfile,sys,http.server,threading,sqlite3
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_run',[P,S,S,S,P],P),('dc_messages',[P,S],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
Event=C.CFUNCTYPE(None,P,S,S);Approve=C.CFUNCTYPE(C.c_int,P,S,S);Cancel=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',Event),('approve',Approve),('cancelled',Cancel)]
allowed=False;reviews=[]
@Event
def event(*args):pass
@Approve
def approve(_,name,body):
 assert name==b'extension.confirm';review=json.loads(body);assert review['package']=='dsh-checkpoint-rewind';assert len(review['question']['options'])==2;reviews.append(review);return int(allowed)
@Cancel
def cancel(*args):return 0
hooks=Hooks(None,event,approve,cancel)
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*a):pass
 def do_POST(self):
  self.rfile.read(int(self.headers['Content-Length']));body=json.dumps({'choices':[{'index':0,'message':{'role':'assistant','content':'Owned fixture turn completed.'},'finish_reason':'stop'}]}).encode();self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
def take(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
try:
 with tempfile.TemporaryDirectory(prefix='dunlin-upstream-rewind-') as tmp:
  home=pathlib.Path(tmp);state=home/'state';state.mkdir(mode=0o700);work=home/'work';work.mkdir();target=work/'owned.txt';target.write_text('before\n');db=lib.dc_open(str(state).encode());assert db
  def req(op,**kw):return take(lib.dc_extensions_call(db,json.dumps(dict(op=op,**kw)).encode(),None))
  def ok(op,**kw):
   value=req(op,**kw);assert value['ok'],value;return value['data']
  def command(name,args,confirm=False):
   return take(lib.dc_extensions_call(db,json.dumps(dict(op='extensions.command',source=row['source'],package=row['package'],version=row['version'],chat='chat',command=name,arguments=args)).encode(),C.byref(hooks) if confirm else None))
  try:
   assert lib.dc_add_chat(db,b'chat',b'Owned checkpoint',str(work).encode());cfg=json.dumps(dict(base_url=f'http://127.0.0.1:{server.server_port}/v1',model='owned',protocol='chat',stream=False,desktop_tools='noTools')).encode();assert take(lib.dc_run(db,b'chat',cfg,b'Owned text baseline',None))['status']=='completed';original=take(lib.dc_messages(db,b'chat'))
   ok('extensions.assets',path=str(pathlib.Path(sys.argv[2]).resolve()));ok('compat.enable',node='/opt/homebrew/bin/node')
   row=ok('extensions.import',path=str(pathlib.Path('build/v51/real-dsh-published/dsh-checkpoint-rewind-0.6.34').resolve()))[0];ok('extensions.bind',source=row['source'],package=row['package'],version=row['version'],enabled=True,acceptHostPermissions=True,config={'provider':'copy','snapshotDir':str(home/'snapshots')})
   value=command('slash:checkpoint',{'input':'note owned native boundary'});assert value['ok'] and 'captured' in json.dumps(value),value
   target.write_text('after\n');value=command('slash:rewind',{'input':'workspace latest'},True);assert value['ok'],value;assert target.read_text()=='after\n';assert reviews,'no native approval callback';assert 'cancel' in json.dumps(value).lower(),value
   allowed=True;value=command('slash:rewind',{'input':'workspace latest'},True);assert value['ok'] and value['data']['data']['result']['kind']=='success',value;assert target.read_text()=='before\n',value;assert take(lib.dc_messages(db,b'chat'))==original,'workspace rewind rewrote native source history'
   with sqlite3.connect(state/'conversations.sqlite') as sql:assert sql.execute('SELECT decision FROM extension_questions ORDER BY created,rowid').fetchall()==[('denied',),('allowed_once',)]
   # Approval without a durable audit must still deny, without a file restore.
   target.write_text('audit must deny\n')
   with sqlite3.connect(state/'conversations.sqlite') as sql:sql.execute("CREATE TRIGGER deny_question BEFORE INSERT ON extension_questions BEGIN SELECT RAISE(ABORT,'owned audit denial'); END")
   value=command('slash:rewind',{'input':'workspace 1'},True);assert not value['ok'] and target.read_text()=='audit must deny\n',value
   assert ok('extensions.faults');assert not ok('extensions.list')[0]['active']
  finally:lib.dc_close(db)
finally:server.shutdown();server.server_close()
print('PASS unchanged checkpoint plugin captures actual C position, denied confirmation leaves files, explicit manual confirmation restores real bytes and keeps native history, missing audit denies restore and disables host. No model command approved.')
