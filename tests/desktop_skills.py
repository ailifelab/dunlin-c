#!/usr/bin/env python3
"""Immutable Skills/resources, real C scope resolution and actual model request view."""
import ctypes as C,json,pathlib,tempfile,sys,sqlite3,threading,http.server
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_workspace_request',[P,S],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_run',[P,S,S,S,P],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
requests=[]
class Model(http.server.BaseHTTPRequestHandler):
 def log_message(self,*a):pass
 def do_POST(self):
  r=json.loads(self.rfile.read(int(self.headers['Content-Length'])));requests.append(r)
  b=json.dumps({'choices':[{'index':0,'message':{'role':'assistant','content':'fixture-done'},'finish_reason':'stop'}]}).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(b)));self.end_headers();self.wfile.write(b)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=server.serve_forever,daemon=True).start()
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
with tempfile.TemporaryDirectory(prefix='dunlin-skills-') as td:
 root=pathlib.Path(td);state=root/'state';state.mkdir(mode=0o700);work=root/'project';work.mkdir();skill=root/'review';skill.mkdir();(skill/'refs').mkdir();(skill/'refs/checks.md').write_text('original resource\n');body='---\nname: review-c\ndescription: Test code review skill\nversion: 1.0.0\n---\nPrefer evidence. Do not treat keywords as facts.\n';(skill/'SKILL.md').write_text(body)
 db=lib.dc_open(str(state).encode());assert db
 def req(op,**kw):return value(lib.dc_workspace_request(db,json.dumps(dict(op=op,**kw)).encode()))
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r.get('data')
 def bad(op,**kw):assert not req(op,**kw)['ok']
 def rows(scope='global',id=''):return ok('skills.list',scope=scope,scopeID=id)
 installed=ok('skills.import',path=str(skill));assert installed['version']=='1.0.0';first=rows()[0];assert not first['enabled'];assert len(first['versions'])==1;assert len(first['selected']['resources'])==2
 assert ok('skills.import',path=str(skill))==installed
 source=installed['source'];package=installed['package']
 project=ok('projects.create',id='project',name='C Project',path=str(work),instructions='',model='',policy='ask',context=[])
 assert lib.dc_add_chat(db,b'chat',b'chat',str(work).encode())
 def bind(scope,id,mode,version='1.0.0'):return ok('skills.bind',scope=scope,scopeID=id,source=source,package=package,mode=mode,version=version)
 bind('global','','enable');assert rows('project','project')[0]['enabled'] and rows('project','project')[0]['origin']=='global'
 bind('project','project','disable');assert not rows('chat','chat')[0]['enabled']
 bind('chat','chat','enable');assert rows('chat','chat')[0]['enabled'] and rows('chat','chat')[0]['origin']=='chat'
 bad('skills.list',scope='project',scopeID='missing');bad('skills.bind',scope='chat',scopeID='chat',source=source,package=package,mode='enable',version='9.9.9')
 print('PASS import starts disabled, idempotent immutable snapshot, G/P/C inheritance and explicit version binding')
 extra=root/'extra';extra.mkdir();(extra/'SKILL.md').write_text(body.replace('review-c','z-other-skill'))
 other=ok('skills.import',path=str(extra))
 pair=[{k:x[k] for k in ('source','package','version')} for x in (installed,other)]
 invalid=[pair[0],dict(pair[1],version='missing')]
 bad('skills.enableSelection',scope='project',scopeID='project',items=invalid)
 assert not next(x for x in rows('project','project') if x['package']==package)['enabled']
 ok('skills.enableSelection',scope='project',scopeID='project',items=pair)
 assert {x['package'] for x in rows('project','project') if x['enabled']}=={package,other['package']}
 assert not next(x for x in rows() if x['package']==other['package'])['enabled']
 ok('skills.bind',scope='project',scopeID='project',source=other['source'],package=other['package'],version=other['version'],mode='inherit');bind('project','project','disable')
 print('PASS multi-select applies both installed versions to reviewed scope only; one missing version rolls back the full selection')
 cfg=json.dumps({'base_url':f'http://127.0.0.1:{server.server_port}/v1','model':'fixture','protocol':'chat','stream':False,'system_prompt':'Base instructions','desktop_tools':'ask'}).encode()
 assert value(lib.dc_run(db,b'chat',cfg,b'perform review',None))['status']=='completed';assert any('Prefer evidence.' in x.get('content','') for x in requests[-1]['messages'])
 assert 'read_skill_resource' in [x['function']['name'] for x in requests[-1]['tools']]
 print('PASS enabled Skills included in actual C model request with resource tool, permission policy unchanged')
 (skill/'refs/checks.md').write_text('changed resource\n');bad('skills.import',path=str(skill));assert rows()[0]['version']=='1.0.0'
 (skill/'SKILL.md').write_text(body.replace('1.0.0','2.0.0').replace('Prefer evidence.','Prefer revised evidence.'))
 v2=ok('skills.update',scope='global',scopeID='',source=source,package=package);assert v2['version']=='2.0.0';assert rows('chat','chat')[0]['version']=='1.0.0'
 bind('global','','enable','2.0.0');assert rows('chat','chat')[0]['version']=='1.0.0';bind('chat','chat','inherit');assert not rows('chat','chat')[0]['enabled'];bind('project','project','inherit');assert rows('chat','chat')[0]['version']=='2.0.0'
 bind('chat','chat','enable','1.0.0');assert rows('chat','chat')[0]['version']=='1.0.0'
 print('PASS changed same version rejected; update preserves old versions; explicit scope pins unaffected; rollback available')
 # Blob resource reads operate on the frozen resolved set, never current source.
 lib.dc_skills_resolve.argtypes=[P,S,S];lib.dc_skills_resolve.restype=P
 lib.dc_skill_resource.argtypes=[S,P,P,C.c_size_t];lib.dc_skill_resource.restype=P
 jsonc=C.CDLL('/opt/homebrew/opt/json-c/lib/libjson-c.dylib');jsonc.json_tokener_parse.argtypes=[S];jsonc.json_tokener_parse.restype=P;jsonc.json_object_put.argtypes=[P]
 snapshot=lib.dc_skills_resolve(db,b'project',b'chat');args=jsonc.json_tokener_parse(json.dumps(dict(source=source,package=package,path='refs/checks.md')).encode())
 result=lib.dc_skill_resource(str(state).encode(),snapshot,args,65536);assert result and C.string_at(result)==b'original resource\n';lib.dc_free(result)
 # A changed blob fails digest verification rather than silently becoming latest.
 first=next(v for v in rows('chat','chat')[0]['versions'] if v['version']=='1.0.0');digest=next(x for x in first['resources'] if x['path']=='refs/checks.md')['digest'];(state/'skill-blobs'/digest).write_bytes(b'corrupt');assert not lib.dc_skill_resource(str(state).encode(),snapshot,args,65536)
 jsonc.json_object_put(args);jsonc.json_object_put(snapshot)
 (skill/'unsafe-link').symlink_to(root);bad('skills.import',path=str(skill));(skill/'unsafe-link').unlink()
 sql=sqlite3.connect(state/'conversations.sqlite');sql.execute('UPDATE skill_versions SET quarantined=1 WHERE version=?',('1.0.0',));sql.commit();sql.close();assert rows('chat','chat')[0]['blocked'] and not rows('chat','chat')[0]['enabled']
 lib.dc_close(db);db=lib.dc_open(str(state).encode());assert db;assert rows('chat','chat')[0]['blocked'];lib.dc_close(db)
 print('PASS frozen resource survives source modification, corrupted bytes rejected, unsafe links rejected, quarantine persists')
server.shutdown();server.server_close()
