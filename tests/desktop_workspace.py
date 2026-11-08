#!/usr/bin/env python3
"""Project lifecycle and actual file/tool integration, isolated own protocol fixture.
No external model, credentials, user workspace or GUI approval automation.
"""
import desktop_fixture_sqlite as fixture_sqlite
import ctypes as C,json,pathlib,tempfile,sys,sqlite3,hashlib,threading,http.server
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
try: api=lib.dc_workspace_request
except AttributeError:
 print('FAIL: missing C project lifecycle API');sys.exit(1)
for name,args,ret in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_workspace_request',[P,S],P),('dc_chats',[P],P),('dc_messages',[P,S],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_delete_chat',[P,S],C.c_int),('dc_run',[P,S,S,S,P],P)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=ret
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
allow=True;proposals=[];requests=[]
@EV
def event(*_):pass
@AP
def approval(ctx,name,args):proposals.append((name.decode(),json.loads(args)));return int(allow)
@CA
def cancelled(_):return 0
hooks=Hooks(None,event,approval,cancelled)
def value(p):
 assert p,'unexpected null response'
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  r=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(r);m=r['messages'];last=m[-1]
  if last['role']=='user' and last['content'].startswith('TOOL '):
   data=json.loads(last['content'][5:].split('\n\n<dunlin_project_references')[0]);msg={'role':'assistant','content':None,'tool_calls':[{'id':'owned-'+str(len(requests)),'type':'function','function':{'name':data['name'],'arguments':json.dumps(data['args'])}}]};finish='tool_calls'
  else:msg={'role':'assistant','content':'fixture-done'};finish='stop'
  body=json.dumps({'choices':[{'index':0,'message':msg,'finish_reason':finish}]}).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='dunlin-projects-') as tmp:
 base=pathlib.Path(tmp);store=base/'private';store.mkdir(mode=0o700);work=base/'work';work.mkdir();nested=work/'src';nested.mkdir();(nested/'main.c').write_text('before\n');(work/'notes.txt').write_text('original reference\n');outside=base/'outside';outside.mkdir();(outside/'secret').write_text('outside-private');(work/'link').symlink_to(outside,target_is_directory=True)
 db=lib.dc_open(str(store).encode());assert db
 def request(op,**kwargs):return value(lib.dc_workspace_request(db,json.dumps(dict(op=op,**kwargs),ensure_ascii=False).encode()))
 def ok(op,**kwargs):
  r=request(op,**kwargs);assert r['ok'],r;return r.get('data')
 def bad(op,**kwargs):
  r=request(op,**kwargs);assert not r['ok'],r;return r
 def project(id='p1',**kw):return dict(id=id,name='验收项目',path=str(work),instructions='Maintain C layering.\nPreserve public APIs.',model='A',policy='ask',context=[],**kw)
 assert ok('projects.list')==[]
 p=ok('projects.create',**project());assert p['revision']==1 and p['available'];assert not (work/'conversations.sqlite').exists()
 bad('projects.create',**project('duplicate'));bad('projects.create',**dict(project('private'),name='Private',path=str(store)))
 bad('projects.create',**dict(project('parent'),name='Parent',path=str(base)))
 bad('projects.create',**dict(project('bad'),name='   '))
 print('PASS project create, C-owned inventory, duplicate/private/ancestor/name rejection')
 assert lib.dc_add_chat(db,b'c1',b'Chat',str(work).encode());assert value(lib.dc_chats(db))[0]['project']=='p1'
 ok('projects.expanded',id='p1',value=False);assert not ok('projects.list')[0]['expanded']
 ok('chats.update',id='c1',field='draft',value='unsent local draft');ok('chats.update',id='c1',field='model',value='B')
 assert ok('chats.metadata',id='c1')=={'draft':'unsent local draft','model':'B'}
 ok('chats.update',id='c1',field='archived',value=True);assert ok('projects.list')[0]['chatCount']==0
 ok('chats.update',id='c1',field='archived',value=False);ok('chats.update',id='c1',field='pinned',value=True)
 p=ok('projects.list')[0];stale=dict(p);p['name']='已改名';p['context']=['notes.txt'];p=ok('projects.update',**p)
 assert p['revision']==2;bad('projects.update',**stale)
 print('PASS persistent collapse, archive/restore/pin, draft/model preferences, optimistic revisions')
 rows=ok('files.list',project='p1',path='');assert 'src' in [x['name'] for x in rows] and 'link' not in [x['name'] for x in rows]
 doc=ok('files.read',project='p1',path='src/main.c');assert doc['sha256']==hashlib.sha256(b'before\n').hexdigest()
 for path in ['../outside/secret','link/secret','src/../notes.txt','/etc/passwd']:
  bad('files.read',project='p1',path=path)
 print('PASS nested project browse, source SHA-256, symlink/traversal/absolute path rejection')
 def run(content,model='A',tools=None):
  cfg={'base_url':f'http://127.0.0.1:{server.server_port}/v1','model':model,'protocol':'chat','stream':False,'system_prompt':'Base system instruction'}
  if tools:cfg['desktop_tools']=tools
  return value(lib.dc_run(db,b'c1',json.dumps(cfg).encode(),content.encode() if content is not None else None,C.byref(hooks)))
 def tool(name,args):return run('TOOL '+json.dumps({'name':name,'args':args}))
 assert run('Use the pinned source')['status']=='completed';assert any('Project instructions:\nMaintain C layering.' in x.get('content','') for x in requests[-1]['messages']);assert any('original reference' in x.get('content','') for x in requests[-1]['messages']);assert value(lib.dc_messages(db,b'c1'))[0]['content']=='Use the pinned source'
 assert run('Switch to model B','B')['status']=='completed';assert any('Use the pinned source' in x.get('content','') for x in requests[-1]['messages'])
 assert tool('write_file',{'path':'src/main.c','text':'after\n','expected_sha256':doc['sha256']})['status']=='completed';assert (nested/'main.c').read_text()=='after\n';assert proposals[-1][0]=='write_file'
 conflict=tool('write_file',{'path':'src/main.c','text':'overwrite','expected_sha256':doc['sha256']});assert conflict['status']=='failed' and 'conflict' in conflict['text'];assert (nested/'main.c').read_text()=='after\n';assert any('conflict' in x['content'] for x in value(lib.dc_messages(db,b'c1')) if x['role']=='tool')
 current=ok('files.read',project='p1',path='src/main.c')
 assert tool('patch_file',{'path':'src/main.c','old_text':'after','new_text':'patched','expected_sha256':current['sha256']})['status']=='completed';assert (nested/'main.c').read_text()=='patched\n'
 allow=False;r=tool('write_file',{'path':'src/denied.txt','text':'no','expected_sha256':'new'});assert not (nested/'denied.txt').exists();allow=True
 print('PASS actual nested approved write/patch; stale hash preserves file; denied write has no effect')
 # New chat isolates ordinary no-tools behavior from the project checkpoint.
 assert lib.dc_add_chat(db,b'ordinary',b'ordinary',str(work).encode())
 ok('chats.update',id='ordinary',field='project',value='')
 cfg=json.dumps({'base_url':f'http://127.0.0.1:{server.server_port}/v1','model':'A','protocol':'chat','stream':False,'desktop_tools':'noTools'}).encode()
 assert value(lib.dc_run(db,b'ordinary',cfg,b'plain chat',C.byref(hooks)))['status']=='completed';assert not requests[-1].get('tools')
 print('PASS ordinary chat request has zero tools')
 # Pending checkpoint cannot be reassigned, reconfigured or project-removed.
 sql=fixture_sqlite.connect(store/'conversations.sqlite');state=json.loads(sql.execute("SELECT state FROM chats WHERE id='c1'").fetchone()[0]);state['active']=True;sql.execute("UPDATE chats SET state=? WHERE id='c1'",(json.dumps(state),));sql.commit()
 bad('projects.update',**p);bad('projects.remove',id='p1',revision=p['revision'],mode='keep');bad('chats.update',id='c1',field='project',value='')
 state['active']=False;sql.execute("UPDATE chats SET state=? WHERE id='c1'",(json.dumps(state),));sql.commit();sql.close()
 print('PASS active-checkpoint barriers prevent workspace/instruction reassignment')
 moved=base/'work-renamed';work.rename(moved);assert not ok('projects.list')[0]['available'];before_requests=len(requests);assert run('must not call model')['status']=='failed';assert len(requests)==before_requests
 p=ok('projects.list')[0];p['path']=str(moved);p=ok('projects.update',**p,relink=True);assert p['available'];assert next(x for x in value(lib.dc_chats(db)) if x['id']=='c1')['workspace']==str(moved.resolve())
 print('PASS disconnected directory retains project; explicit relink atomically updates chat workspace')
 lib.dc_close(db);db=lib.dc_open(str(store).encode());assert db;assert ok('projects.list')[0]['name']=='已改名';assert ok('chats.metadata',id='c1')['draft']=='unsent local draft';assert not ok('projects.list')[0]['expanded']
 ok('projects.remove',id='p1',revision=p['revision'],mode='archive');assert ok('projects.list')==[];chat=next(x for x in value(lib.dc_chats(db)) if x['id']=='c1');assert chat['archived'] and chat['project']=='';assert (moved/'src/main.c').read_text()=='patched\n'
 lib.dc_close(db)
 print('PASS restart persistence and remove/archive preserving actual project files')
 # Retired migration requests cannot recreate project membership or content.
 isolated=base/'migration-rejected';isolated.mkdir(mode=0o700);db=lib.dc_open(str(isolated).encode());assert lib.dc_add_chat(db,b'ordinary',b'ordinary',str(moved).encode())
 assert not request('projects.migrate',paths=[str(moved)])['ok'];assert ok('projects.list')==[];assert value(lib.dc_chats(db))[0]['project']=='';lib.dc_close(db)
 print('PASS retired automatic path migration rejected without creating project membership')
server.shutdown();server.server_close()
