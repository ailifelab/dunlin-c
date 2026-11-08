"""Real owned process death freezes SDK catalog; config/version/disable changes
must block resume before a model request or plugin tool effect.
"""
import ctypes as C,json,pathlib,tempfile,sys,http.server,threading,subprocess,os
library=str(pathlib.Path(sys.argv[1]).resolve());assets=str(pathlib.Path(sys.argv[2]).resolve());lib=C.CDLL(library);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_run',[P,S,S,S,P],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
def take(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
def request(db,op,**args):
 r=take(lib.dc_extensions_call(db,json.dumps(dict(op=op,**args)).encode(),None));assert r['ok'],r;return r['data']
if len(sys.argv)>3 and sys.argv[3]=='--child':
 db=lib.dc_open(sys.argv[4].encode());assert db;request(db,'extensions.assets',path=assets);request(db,'extensions.restore');print(take(lib.dc_run(db,b'chat',sys.argv[5].encode(),b'owned interrupted model',None)),flush=True);lib.dc_close(db);raise SystemExit(0)
started=threading.Event();release=threading.Event();seen=[]
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  seen.append(json.loads(self.rfile.read(int(self.headers['Content-Length']))));started.set();release.wait(20);raw=json.dumps({'choices':[{'index':0,'message':{'role':'assistant','content':'Owned model response'},'finish_reason':'stop'}]}).encode();self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.end_headers()
  try:self.wfile.write(raw)
  except (BrokenPipeError,ConnectionResetError):pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
try:
 with tempfile.TemporaryDirectory(prefix='dunlin-extension-resume-') as tmp:
  root=pathlib.Path(tmp);state=root/'state';state.mkdir(mode=0o700);work=root/'workspace';work.mkdir();package=root/'package';package.mkdir();effect=work/'effect.txt';other=work/'other.txt';db=lib.dc_open(str(state).encode());assert db
  manifest=dict(protocol='dunlin.extension',major=1,packageId='resume',version='1',kind='plugin',engines={'dunlin':'desktop@1','host':'node-dsh','dsh':'0.2.1-rc.3'},entry='index.ts',capabilities=['trusted-full-host'],requires=[],dependencies=[])
  (package/'index.ts').write_text("import{writeFile}from'node:fs/promises';import{defineTool}from'@deepseek-ai/dsh-tools';export const inject=['tools'];export function apply(ctx,config){ctx.tools.register(defineTool({name:'owned_write',description:'Owned write',parameters:{},output:{schema:{type:'string'},render:(_a,v)=>[{type:'text',text:v}]},async execute(){await writeFile(config.target,'effect');return 'written'}}))}")
  def import_version(version):
   manifest['version']=version;(package/'dunlin.json').write_text(json.dumps(manifest));return request(db,'extensions.import',path=str(package))[0]
  def bind(version='1',enabled=True,target=effect):return request(db,'extensions.bind',source=row['source'],package=row['package'],version=version,enabled=enabled,acceptHostPermissions=True,config={'target':str(target)})
  request(db,'extensions.assets',path=assets);request(db,'compat.enable',node='/opt/homebrew/bin/node');row=import_version('1');bind();assert lib.dc_add_chat(db,b'chat',b'Owned interruption',str(work).encode());lib.dc_close(db)
  settings=json.dumps(dict(base_url=f'http://127.0.0.1:{server.server_port}/v1',model='owned',protocol='chat',stream=False,desktop_tools='ask'))
  child=subprocess.Popen([os.environ.get('DUNLIN_TEST_PYTHON',sys.executable),'-B',__file__,library,assets,'--child',str(state),settings],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  try:
   if not started.wait(10):
    child.kill();output,error=child.communicate(timeout=10);raise AssertionError(('owned child failed before model request',output.decode(errors='replace'),error.decode(errors='replace')))
   assert any(t['function']['name'].startswith('ext_') for t in seen[0]['tools']);child.kill();child.communicate(timeout=10)
  finally:
   if child.poll() is None:child.kill();child.communicate(timeout=10)
   release.set()
  db=lib.dc_open(str(state).encode());assert db
  try:
   request(db,'extensions.assets',path=assets);request(db,'extensions.restore')
   def blocked():
    count=len(seen);r=take(lib.dc_run(db,b'chat',settings.encode(),None,None));assert r['status']=='busy',r;assert len(seen)==count and not effect.exists() and not other.exists()
   bind(target=other);blocked();bind();bind(enabled=False);blocked();import_version('2');bind(version='2');blocked()
  finally:lib.dc_close(db)
finally:release.set();server.shutdown();server.server_close()
print('PASS actual process kill with frozen SDK catalog; config change, disabled binding and new version each block before model/network/tool effects. No model command approved.')
