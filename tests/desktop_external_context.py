"""Actual extension import, activation, summary request, reference selection and version refusal."""
import ctypes as C
import http.server
import json
from pathlib import Path
import sys
import tempfile
import shutil
import threading

lib = C.CDLL(sys.argv[1])
P,S=C.c_void_p,C.c_char_p
for name,args,ret in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),
                      ('dc_workspace_request',[P,S],P),('dc_extensions_call',[P,S,P],P),
                      ('dc_add_chat',[P,S,S,S],C.c_int),('dc_run',[P,S,S,S,P],P)]:
    fn=getattr(lib,name); fn.argtypes=args; fn.restype=ret

def value(raw):
    assert raw
    try: return json.loads(C.string_at(raw))
    finally: lib.dc_free(raw)

requests=[]
class Model(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_POST(self):
        requests.append(json.loads(self.rfile.read(int(self.headers['Content-Length']))))
        body=b'{"choices":[{"index":0,"message":{"role":"assistant","content":"ok"},"finish_reason":"stop"}]}'
        self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model)
threading.Thread(target=server.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory() as tmp:
    state=Path(tmp)/"data"; state.mkdir(mode=0o700); work=Path(tmp)/"work"; work.mkdir()
    db=lib.dc_open(str(state).encode()); assert db
    def ext(op,**kw):
        r=value(lib.dc_extensions_call(db,json.dumps(dict(op=op,**kw)).encode(),None));assert r['ok'],r;return r['data']
    def core(op,**kw):
        r=value(lib.dc_workspace_request(db,json.dumps(dict(op=op,**kw)).encode()));assert r['ok'],r;return r['data']
    ext('extensions.assets',path=str(Path(sys.argv[2]).resolve()))
    ext('python.configure',executable=sys.executable)
    row=ext('extensions.import',path=str(Path('compat/context/extractive').resolve()))[0]
    ref={k:row[k] for k in ('source','package','version')}
    assert not row['enabled'];ext('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)
    assert lib.dc_add_chat(db,b'chat',b'Sources',str(work).encode())
    policy=dict(modelLimit=4096,deploymentLimit=0,selectedLimit=4096,outputLimit=256,requestedOutput=128,
                reserve=128,strategy='external',external=ref,automatic=True,threshold=.5,retainTurns=2,summaryOutput=128)
    def run(text, chat=b'chat', hooks=None):
        cfg=dict(base_url=f'http://127.0.0.1:{server.server_port}/v1',model='fixture',protocol='chat',stream=False,
                 desktop_tools='noTools',max_tokens=128,desktop_context=policy)
        return value(lib.dc_run(db,chat,json.dumps(cfg).encode(),text.encode(),hooks))
    for i in range(7):
        result=run(f'round-{i} '+('older text '*45));assert result['status']=='completed',result
    row=core('context.records',chat='chat')[0]
    assert row['strategy'].startswith('external:') and row['status']=='committed'
    choices=core('context.sources',chat='chat',id=row['id']);assert choices
    choice=choices[0];policy['reinclude']=dict(record=row['id'],digests=[str(choice['seq'])+':'+choice['digest']])
    assert run('Recall the source')['status']=='completed'
    assert any(choice['text'] in m.get('content','') and 'Previously quoted' in m['content'] for m in requests[-1]['messages'])
    assert not core('context.sources',chat='another',id=row['id'])
    policy['reinclude']['digests']=['missing'];before=len(requests)
    assert run('invalid')['status']!='completed' and len(requests)==before
    del policy['reinclude'];ext('extensions.bind',**ref,enabled=False)
    assert run('disabled')['status']!='completed' and len(requests)==before

    # Cancel after the strategy starts. Its return value must never enter the
    # summary cache, and unrelated commands sharing the worker stay usable.
    gate = Path(tmp)/'started'
    slow = Path(tmp)/'slow-strategy'
    shutil.copytree('compat/context/extractive', slow)
    manifest = slow/'dunlin.json'
    data = json.loads(manifest.read_text())
    data['packageId'] = 'slow-extractor'
    manifest.write_text(json.dumps(data))
    code = slow/'strategy.py'
    code.write_text(code.read_text().replace('    messages = arguments',
        f'    import time\n    open({str(gate)!r}, "a").close()\n    time.sleep(0.15)\n    messages = arguments'))
    slow_row = next(r for r in ext('extensions.import', path=str(slow)) if r['package'] == 'slow-extractor')
    slow_ref = {k:slow_row[k] for k in ('source','package','version')}
    ext('extensions.bind', **slow_ref, enabled=True, acceptHostPermissions=True)
    ext('extensions.bind', **ref, enabled=True, acceptHostPermissions=True)
    policy['external'] = slow_ref
    assert lib.dc_add_chat(db,b'cancel',b'Cancel',str(work).encode())
    cancel_callback = C.CFUNCTYPE(C.c_int,P)
    class Hooks(C.Structure):
        _fields_ = [('user',P),('event',P),('approve',P),('cancelled',cancel_callback)]
    callbacks = Hooks(None,None,None,cancel_callback(lambda _: int(gate.exists())))
    for i in range(9):
        before = len(requests)
        outcome = run(f'round-{i} '+('older text '*45), b'cancel', C.byref(callbacks))
        if gate.exists(): break
        assert outcome['status'] == 'completed'
    assert gate.exists() and outcome['status'] == 'cancelled', (gate.exists(), outcome, core('context.records', chat='cancel'))
    assert len(requests) == before
    assert core('context.records', chat='cancel')[0]['status'] == 'cancelled'
    for strategy in (ref, slow_ref):
        result = ext('extensions.command', **strategy, command='compress_context',
                     arguments=dict(messages=[dict(role='user',content='retained')]))
        assert 'retained' in result['data']['summary']
    print('PASS cancellation during live external strategy discards reply, records cancelled and retains shared worker')
    lib.dc_close(db)
server.shutdown();server.server_close()
print('PASS installed external Python strategy runs in worker; original quotation reaches model; foreign, stale and disabled selections denied')
