#!/usr/bin/env python3
"""Owned corrupted model options remain editable, while execution blocks."""
import ctypes as C,pathlib,sys,json,tempfile
import desktop_fixture_sqlite as fixture_sqlite
P=C.c_void_p;S=C.c_char_p
def load(path):
 lib=C.CDLL(str(pathlib.Path(path).resolve()))
 for n,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_workspace_request',[P,S],P)]:f=getattr(lib,n);f.argtypes=args;f.restype=result
 return lib
current=load(sys.argv[1])
def req(lib,db,op,**kw):
 p=lib.dc_workspace_request(db,json.dumps(dict(op=op,**kw)).encode());assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
with tempfile.TemporaryDirectory(prefix='dunlin-model-options-corruption-') as tmp:
 path=pathlib.Path(tmp)/'private';path.mkdir(mode=0o700);db=current.dc_open(str(path).encode());assert db
 base=dict(id='normal',name='Normal',endpoint='https://example.invalid/v1',model='fixture',protocolName='chat',maxOutput=4096)
 invalidStored=dict(base,id='invalid-stored',name='Invalid stored',protocolName='anthropic',enableThinking=False)
 assert req(current,db,'config.save',revision=0,models=[base],providers=[])['ok'];current.dc_close(db)
 with fixture_sqlite.connect(path/'conversations.sqlite') as sql:
  sql.execute('UPDATE configuration_registry SET models=? WHERE singleton=1',(json.dumps([base,invalidStored]),))
 db=current.dc_open(str(path).encode());assert db
 reply=req(current,db,'config.list');assert reply['ok'],('invalid stored model options must not hide every model',reply)
 registry=reply['data'];assert registry['modelIssues']==[dict(id='invalid-stored',error='unsupported_request_options')],registry
 assert req(current,db,'config.model',id='normal',revision=registry['revision'])['ok']
 assert req(current,db,'config.model',id='invalid-stored',revision=registry['revision'])['error']=='configuration_model_options_unsupported'
 renamed=dict(base,name='Renamed normal');reply=req(current,db,'config.save',revision=registry['revision'],models=[renamed,invalidStored],providers=[]);assert reply['ok'],reply;registry=reply['data']
 bad=dict(invalidStored,endpoint='https://another.invalid/v1');assert not req(current,db,'config.save',revision=registry['revision'],models=[renamed,bad],providers=[])['ok']
 assert not req(current,db,'config.preview',source=json.dumps(dict(base_url=base['endpoint'],model='bad-new-route',protocol='anthropic',max_tokens=4096,enable_thinking=False)))['ok']
 fixed={k:v for k,v in invalidStored.items() if k!='enableThinking'};reply=req(current,db,'config.save',revision=registry['revision'],models=[renamed,fixed],providers=[]);assert reply['ok'] and reply['data']['modelIssues']==[],reply
 assert req(current,db,'config.model',id='invalid-stored',revision=reply['data']['revision'])['ok'];current.dc_close(db)
 print('PASS owned corrupt registry retains blocked route and unrelated model access; explicit mapper rejection, unchanged neighbor publication, no new invalid route, explicit repair')
