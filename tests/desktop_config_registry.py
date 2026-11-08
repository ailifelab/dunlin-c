"""Isolated C configuration registry; no credentials, services or user state."""
import desktop_fixture_sqlite as fixture_sqlite
import ctypes as C
import json
import pathlib
import sqlite3
import sys
import tempfile

lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
lib.dc_open.argtypes=[C.c_char_p];lib.dc_open.restype=C.c_void_p
lib.dc_close.argtypes=[C.c_void_p]
lib.dc_free.argtypes=[C.c_void_p]
lib.dc_workspace_request.argtypes=[C.c_void_p,C.c_char_p];lib.dc_workspace_request.restype=C.c_void_p
lib.dc_add_chat.argtypes=[C.c_void_p,C.c_char_p,C.c_char_p,C.c_char_p];lib.dc_add_chat.restype=C.c_int
lib.dc_read_view.argtypes=[C.c_char_p,C.c_char_p];lib.dc_read_view.restype=C.c_void_p
def decode(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
with tempfile.TemporaryDirectory(prefix='dunlin-config-') as tmp:
 root=pathlib.Path(tmp);state=root/'private';state.mkdir(mode=0o700);work=root/'project';work.mkdir()
 db=lib.dc_open(str(state).encode());assert db
 def request(op,**kw):return decode(lib.dc_workspace_request(db,json.dumps(dict(op=op,**kw)).encode()))
 def ok(op,**kw):
  v=request(op,**kw);assert v['ok'],v;return v.get('data')
 def bad(op,**kw):
  v=request(op,**kw);assert not v['ok'],v;return v['error']
 model=dict(id='model-A',name='Fixture A',endpoint='http://127.0.0.1:8080/v1',model='fixture-A',protocolName='chat',maxOutput=4096)
 assert ok('config.list')==dict(revision=0,models=[],providers=[],connectionTests=[],modelIssues=[])
 assert bad('config.bootstrap',models=[model],providers=[])=='configuration_operation_unknown'
 assert ok('config.list')['revision']==0
 registry=ok('config.save',revision=0,models=[model],providers=[]);assert registry['revision']==1 and registry['models']==[model]
 assert bad('config.bootstrap',models=[],providers=[])=='configuration_operation_unknown'
 assert ok('config.list')==registry
 bad('config.save',revision=0,models=[],providers=[])
 for changes in [dict(endpoint='https://u:secret@example.com/v1'),dict(endpoint='https://example.com/v1?api_key=secret'),dict(endpoint='file:///etc/passwd'),dict(maxOutput=True),dict(providerID=3),dict(credentialID='../wrong'),dict(contextLimit=1024),dict(api_key='must-not-save')]:
  bad('config.save',revision=1,models=[dict(model,**changes)],providers=[])
 provider=dict(id='provider-A',name='Fixture provider',endpoint=model['endpoint'],protocolName='chat',credentialRevision=1,credentialID='credential-version-1')
 linked=dict(model,providerID='provider-A',credentialRevision=1,credentialID='credential-version-1')
 registry=ok('config.save',revision=1,models=[linked],providers=[provider]);assert registry['revision']==2
 bad('config.save',revision=2,models=[linked],providers=[])
 bad('config.save',revision=2,models=[dict(linked,credentialID='different')],providers=[provider])
 assert lib.dc_add_chat(db,b'active-chat',b'Guard fixture',str(work).encode())
 ok('chats.update',id='active-chat',field='model',value=model['id'])
 with fixture_sqlite.connect(state/'conversations.sqlite') as sql:
  sql.execute('UPDATE chats SET state=? WHERE id=?',(json.dumps(dict(active=True,messages=[])),'active-chat'))
 bad('config.save',revision=2,models=[dict(linked,model='changed')],providers=[provider])
 bad('config.save',revision=2,models=[],providers=[provider])
 registry=ok('config.save',revision=2,models=[dict(linked,name='Renamed only')],providers=[provider]);assert registry['revision']==3
 print('PASS retired bootstrap rejection and current registry restart identity, revision CAS, strict types/URLs/budgets, provider references and active route guard')
 source=dict(base_url='https://example.com/v1',max_tokens=4096,api_key='FIXTURE_SECRET_MUST_NOT_PERSIST',models={'A':dict(model='upstream-A'),'B':dict(model='upstream-B',protocol='responses')})
 plan=ok('config.preview',source=json.dumps(source));assert len(plan['models'])==2 and plan['warnings'];assert ok('config.list')==registry
 assert all(x['id']!='model-A' for x in plan['models'])
 bad('config.apply',plan=plan['plan'],confirmed=False)
 bad('config.apply',plan=plan['plan'],confirmed='true')
 updated=ok('config.apply',plan=plan['plan'],confirmed=True);assert len(updated['models'])==3 and updated['revision']==4
 bad('config.apply',plan=plan['plan'],confirmed=True)
 plan=ok('config.preview',source=json.dumps(source));registry=ok('config.save',revision=4,models=updated['models'],providers=updated['providers']);bad('config.apply',plan=plan['plan'],confirmed=True)
 plan=ok('config.preview',source=json.dumps(source))
 with fixture_sqlite.connect(state/'conversations.sqlite') as sql:sql.execute('UPDATE configuration_imports SET expires=0 WHERE id=?',(plan['plan'],))
 bad('config.apply',plan=plan['plan'],confirmed=True)
 for source in ['!!js > process.exit()',json.dumps(dict(include='other.json')),json.dumps(dict(plugins=['arbitrary-code'])),json.dumps(dict(models={'X':dict(base_url='http://localhost',model='X',unknown_field=1)})),json.dumps(dict(base_url='http://localhost',model='X',max_tokens=0)), '{"model":"A","model":"B"}']:
  bad('config.preview',source=source)
 bad('config.preview',source='x'*1048577)
 assert not lib.dc_workspace_request(db,json.dumps(dict(op='projects.list',extra='x'*131072)).encode())
 large=dict(models={str(i):dict(base_url='http://localhost/v1',model=str(i),system_prompt='x'*60000) for i in range(3)})
 large_plan=ok('config.preview',source=json.dumps(large));assert len(large_plan['models'])==3 and len(large_plan['models'][0]['systemPrompt'])==60000
 print('PASS preview is inert, explicit typed one-use application, expiry/revision invalidation and dynamic/unsupported input refusal')
 exported=dict(format='dunlin.config',version=1,models=[linked],providers=[provider]);plan=ok('config.preview',source=json.dumps(exported));assert plan['providers'][0]['id']!='provider-A';assert 'credentialID' not in plan['providers'][0] and 'credentialID' not in plan['models'][0];assert plan['models'][0]['providerID']==plan['providers'][0]['id'];assert plan['models'][0]['credentialRevision']==0
 updated=ok('config.apply',plan=plan['plan'],confirmed=True)
 raw=json.dumps(dict(op='config.list')).encode();assert decode(lib.dc_read_view(str(state).encode(),raw))['data']==updated
 assert not decode(lib.dc_read_view(str(state).encode(),json.dumps(dict(op='config.apply',plan='invalid',confirmed=True)).encode()))['ok']
 with fixture_sqlite.connect(state/'conversations.sqlite') as sql:assert 'FIXTURE_SECRET_MUST_NOT_PERSIST' not in '\n'.join(sql.iterdump())
 lib.dc_close(db);db=lib.dc_open(str(state).encode());assert db;assert ok('config.list')==updated;lib.dc_close(db)
 print('PASS portable import creates fresh route/credential identities, original active chat preserved, read-only view and persisted data contain no supplied credential values')
