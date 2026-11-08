#!/usr/bin/env python3
"""Minimal provider probes through real C protocol adapters; loopback fixture only.
No model service, user credentials, tool execution or user data are involved.
"""
import ctypes as C, json, pathlib, tempfile, sys, threading, http.server, sqlite3, time, subprocess, os
library = pathlib.Path(sys.argv[1]).resolve()
lib = C.CDLL(str(library)); P=C.c_void_p; S=C.c_char_p
Cancel=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):
    _fields_=[('user',P),('event',P),('approve',P),('cancelled',Cancel)]
for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_test_connection',[P,S,S,P],P),('dc_workspace_request',[P,S],P),('dc_chats',[P],P)]:
    fn=getattr(lib,name);fn.argtypes=args;fn.restype=result
def value(pointer):
    assert pointer
    try:return json.loads(C.string_at(pointer))
    finally:lib.dc_free(pointer)
def encode(v):return json.dumps(v,ensure_ascii=False).encode()
def call(db,p,secret=b'',hooks=None):return value(lib.dc_test_connection(db,encode(p),secret,C.byref(hooks) if hooks else None))
if '--child' in sys.argv:
    db=lib.dc_open(sys.argv[3].encode());assert db
    call(db,json.loads(sys.argv[4]));raise SystemExit('probe returned before kill')

seen=[];errors=[];mode='ok';entered=threading.Event();release=threading.Event()
class Server(http.server.BaseHTTPRequestHandler):
    def log_message(self,*_):pass
    def do_POST(self):
        try:
            request=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            seen.append(dict(path=self.path,headers=dict(self.headers),body=request))
            assert request.get('stream',False) is False and not request.get('tools'),request
            if mode=='hold':
                entered.set();release.wait(10)
            status=302 if mode=='redirect' else 401 if mode=='unauthorized' else 429 if mode=='rate' else 200
            if mode in ('redirect','unauthorized','rate'):response={'error':{'message':'FIXTURE_ERROR_BODY_NOT_SAVED'}}
            elif mode=='malformed':response={'unexpected':'not a protocol response'}
            elif mode=='large':response={'body':'X'*70000}
            elif self.path.endswith('/responses'):response=dict(id='response-fixture',status='completed',output=[dict(type='message',id='message-fixture',role='assistant',status='completed',content=[dict(type='output_text',text='OK',annotations=[])])])
            elif self.path.endswith('/messages'):response=dict(id='message-fixture',type='message',role='assistant',content=[dict(type='text',text='OK')],stop_reason='end_turn',usage=dict(input_tokens=4,output_tokens=1))
            elif self.path.endswith('/api/chat'):response=dict(model='fixture',message=dict(role='assistant',content='OK'),done=True,done_reason='stop')
            elif ':generateContent' in self.path:response=dict(candidates=[dict(content=dict(role='model',parts=[dict(text='OK')]),finishReason='STOP')])
            else:
                message=dict(role='assistant',content=' ' if mode=='empty' else 'OK')
                if mode=='tool':message=dict(role='assistant',content=None,tool_calls=[dict(id='fixture-call',type='function',function=dict(name='shell',arguments='{"command":"must not execute"}'))])
                response=dict(choices=[dict(index=0,message=message,finish_reason='tool_calls' if mode=='tool' else 'stop')])
            body=encode(response);self.send_response(status)
            if status==302:self.send_header('Location','http://127.0.0.1:1/do-not-follow')
            self.send_header('Content-Length',str(len(body)));self.end_headers()
            try:self.wfile.write(body)
            except (BrokenPipeError,ConnectionResetError):pass
        except BaseException as error:errors.append(error);self.close_connection=True
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Server);threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='dunlin-minimal-probe-') as tmp:
    state=pathlib.Path(tmp)/'private';state.mkdir(mode=0o700);db=lib.dc_open(str(state).encode());assert db
    def req(op,**kw):return value(lib.dc_workspace_request(db,encode(dict(op=op,**kw))))
    def ok(op,**kw):
        reply=req(op,**kw);assert reply['ok'],reply;return reply['data']
    endpoint=f'http://127.0.0.1:{server.server_port}/v1'
    provider=dict(id='fixture-provider',routeID='local',name='本机测试',endpoint=endpoint,protocolName='chat',authentication='none',testModel='fixture',credentialRevision=0)
    registry=ok('config.save',revision=0,models=[],providers=[provider]);assert registry['connectionTests']==[]
    for protocol,path in [('chat','/v1/chat/completions'),('responses','/v1/responses'),('anthropic','/v1/messages'),('ollama','/api/chat'),('gemini','/v1/models/fixture:generateContent')]:
        p=dict(provider,protocolName=protocol,endpoint=endpoint.removesuffix('/v1')+'/api' if protocol=='ollama' else endpoint);reply=call(db,p);assert reply['ok'] and reply['data']['status']=='passed',(protocol,reply,seen[-1] if seen else errors)
        assert seen[-1]['path']==path and not seen[-1]['headers'].get('Authorization') and not seen[-1]['headers'].get('x-api-key') and not seen[-1]['headers'].get('x-goog-api-key'),seen[-1]
        assert reply['data']['httpStatus']==200 and reply['data']['durationMs']>=0 and reply['data']['requestBytes']>0 and reply['data']['responseBytes']>0
    assert value(lib.dc_chats(db))==[]
    assert len(ok('config.list')['connectionTests'])==1
    print('PASS explicit minimal text generation in all five protocols, bounded request, original model ID, no tools/chats and actual optional HTTP/time/byte metadata')
    key=b'OWNED_FAKE_KEY_NOT_A_USER_CREDENTIAL'
    for protocol,auth,header in [('chat','auto','Authorization'),('anthropic','auto','x-api-key'),('gemini','auto','x-goog-api-key'),('gemini','bearer','Authorization')]:
        p=dict(provider,protocolName=protocol,authentication=auth,credentialID='fake-key-ref',credentialRevision=3)
        reply=call(db,p,key);assert reply['ok'] and reply['data']['status']=='passed',reply
        assert seen[-1]['headers'][header]==('Bearer ' if header=='Authorization' else '')+key.decode(),seen[-1]
    before=len(seen);assert not call(db,provider,key)['ok'] and len(seen)==before
    invalid=dict(provider,apiKey='not accepted');assert not call(db,invalid)['ok'] and len(seen)==before
    for fixture,expected in [('unauthorized','test_http_error'),('redirect','test_http_error'),('rate','test_rate_limit'),('malformed','test_protocol_error'),('empty','test_no_text'),('tool','test_unexpected_tool_call'),('large','test_response_limit')]:
        mode=fixture;reply=call(db,provider);assert reply['ok'] and reply['data']['status']=='failed' and reply['data']['error']==expected,(fixture,reply)
        if fixture in ('unauthorized','redirect','rate'):assert 'responseBytes' not in reply['data'],reply
    mode='ok';callback=Cancel(lambda _:1);hooks=Hooks(None,None,None,callback);before=len(seen);reply=call(db,provider,hooks=hooks);assert reply['ok'] and reply['data']['status']=='cancelled' and len(seen)==before and 'httpStatus' not in reply['data'],reply
    print('PASS native auth modes, invalid config blocked before HTTP, no redirects, protocol/HTTP/rate/empty/tool/response-limit errors and pre-request cancellation')
    # Slow network cancellation while the owned C invocation is live.
    mode='hold';entered.clear();release.clear();stop=threading.Event();callback=Cancel(lambda _:int(stop.is_set()));hooks=Hooks(None,None,None,callback);out=[]
    thread=threading.Thread(target=lambda:out.append(call(db,provider,hooks=hooks)));thread.start();assert entered.wait(3);stop.set();thread.join(4);release.set();assert not thread.is_alive() and out[0]['data']['status']=='cancelled',out
    mode='ok';reply=call(db,provider);passed=reply['data'];registry=ok('config.list');assert registry['connectionTests'][0]['id']==passed['id']
    renamed=dict(provider,name='新名称',routeID='renamed');registry=ok('config.save',revision=registry['revision'],models=[],providers=[renamed]);assert registry['connectionTests'][0]['id']==passed['id']
    changed=dict(renamed,testModel='another-model');registry=ok('config.save',revision=registry['revision'],models=[],providers=[changed]);assert registry['connectionTests']==[]
    # A successful test from a different credential version is never adopted.
    authed=dict(provider,authentication='auto',credentialID='fake-key-ref',credentialRevision=1);reply=call(db,authed,key);registry=ok('config.save',revision=registry['revision'],models=[],providers=[authed]);assert registry['connectionTests'][0]['id']==reply['data']['id']
    registry=ok('config.save',revision=registry['revision'],models=[],providers=[dict(authed,credentialRevision=2)]);assert registry['connectionTests']==[]
    lib.dc_close(db);db=lib.dc_open(str(state).encode());assert db;assert ok('config.list')==registry
    print('PASS in-flight cancellation, durable exact-configuration status, harmless rename and model/credential revision invalidation across restart')
    with sqlite3.connect(state/'conversations.sqlite') as sql:
        dump='\n'.join(sql.iterdump());assert key.decode() not in dump and 'FIXTURE_ERROR_BODY_NOT_SAVED' not in dump and 'Reply with OK.' not in dump
        assert sql.execute('SELECT COUNT(*) FROM chats').fetchone()[0]==0
        # Failed admission must not issue a request; no orphan success status.
        sql.execute("CREATE TRIGGER fail_probe BEFORE INSERT ON connection_tests BEGIN SELECT RAISE(ABORT,'owned fixture'); END")
    before=len(seen);assert call(db,provider)['error']=='test_storage_error' and len(seen)==before
    with sqlite3.connect(state/'conversations.sqlite') as sql:
        sql.execute('DROP TRIGGER fail_probe');sql.execute("CREATE TRIGGER fail_probe_result BEFORE UPDATE ON connection_tests WHEN NEW.status<>'running' BEGIN SELECT RAISE(ABORT,'owned fixture'); END")
    reply=call(db,provider);assert reply['error']=='test_result_storage_error',reply
    with sqlite3.connect(state/'conversations.sqlite') as sql:sql.execute('DROP TRIGGER fail_probe_result')
    lib.dc_close(db);db=lib.dc_open(str(state).encode());assert db
    with sqlite3.connect(state/'conversations.sqlite') as sql:assert sql.execute("SELECT count(*) FROM connection_tests WHERE status='running'").fetchone()[0]==0
    lib.dc_close(db)
    if not os.environ.get('DUNLIN_SKIP_CRASH_CHILD'):
        mode='hold';entered.clear();release.clear()
        child=subprocess.Popen([sys.executable,'-B',__file__,str(library),'--child',str(state),json.dumps(provider)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            assert entered.wait(4),'owned child did not reach HTTP fixture'
            with sqlite3.connect(state/'conversations.sqlite') as sql:
                pending=sql.execute("SELECT id FROM connection_tests WHERE status='running'").fetchall();assert len(pending)==1
            child.kill();child.communicate(timeout=3);assert child.returncode<0
        finally:
            release.set()
            if child.poll() is None:child.kill();child.communicate(timeout=3)
        db=lib.dc_open(str(state).encode());assert db
        with sqlite3.connect(state/'conversations.sqlite') as sql:
            row=sql.execute('SELECT status,error,http_status,duration_ms FROM connection_tests WHERE id=?',pending[0]).fetchone();assert row==('interrupted','test_interrupted',None,None),row
        lib.dc_close(db)
    assert not errors,errors
    print('PASS secret/response exclusion, durable admission before HTTP, final storage failure and interrupted intent without invented timing/status or replay; crash child '+('skipped' if os.environ.get('DUNLIN_SKIP_CRASH_CHILD') else 'verified'))
server.shutdown()
