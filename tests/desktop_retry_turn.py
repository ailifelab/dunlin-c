#!/usr/bin/env python3
"""Retry after actual summary failure or cancellation without duplicating the last user turn."""
import ctypes as C
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import sys
import tempfile
import threading


lib = C.CDLL(sys.argv[1])
P, S = C.c_void_p, C.c_char_p
Event = C.CFUNCTYPE(None, P, S, S)
Approval = C.CFUNCTYPE(C.c_int, P, S, S)
Cancel = C.CFUNCTYPE(C.c_int, P)


class Hooks(C.Structure):
    _fields_ = [('user', P), ('event', Event), ('approve', Approval), ('cancel', Cancel)]


for name, args, result in [
    ('dc_open', [S], P), ('dc_close', [P], None), ('dc_free', [P], None),
    ('dc_add_chat', [P, S, S, S], C.c_int), ('dc_messages', [P, S], P),
    ('dc_workspace_request', [P, S], P), ('dc_retry_chat_turn', [P, S, S, S, P, S], P),
    ('dc_run_message_with_secret', [P, S, S, S, P, S, S, C.POINTER(C.c_int)], P),
]:
    f = getattr(lib, name)
    f.argtypes, f.restype = args, result


def unpack(pointer):
    assert pointer
    try:
        return json.loads(C.string_at(pointer))
    finally:
        lib.dc_free(pointer)


def encode(value):
    return json.dumps(value).encode()


class Model(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        summary = 'DUNLIN_CONTEXT_CHECKPOINT_V1:' in request['messages'][-1]['content']
        self.server.requests.append('summary' if summary else 'model')
        if summary and self.server.mode == 'fail':
            self.send_error(503)
            return
        if summary and self.server.mode == 'hold':
            self.server.entered.set()
            self.server.release.wait(10)
        body = encode(dict(choices=[dict(index=0, message=dict(role='assistant', content='answer'),
                                          finish_reason='stop')]))
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


def check(server, work, handle, scenario):
    chat = scenario.encode()
    assert lib.dc_add_chat(handle, chat, chat, str(work).encode())
    events, reviews = [], []
    server.mode = 'ok'
    server.entered.clear()
    server.release.clear()
    hooks = Hooks(None, Event(lambda _, key, text: events.append(key.decode())),
                  Approval(lambda *_: reviews.append(True) or 0),
                  Cancel(lambda _: server.mode == 'hold' and server.entered.is_set()))
    config = dict(base_url=f'http://127.0.0.1:{server.server_port}/v1', stream=False,
                  protocol='chat', model='fixture', max_tokens=128, desktop_tools='noTools')

    def run(text):
        committed = C.c_int()
        raw = lib.dc_run_message_with_secret(handle, chat, encode(config), text, C.byref(hooks),
                                               b'', None, C.byref(committed))
        return unpack(raw), bool(committed.value)

    def retry(token):
        return unpack(lib.dc_retry_chat_turn(handle, chat, encode(config), token.encode(),
                                             C.byref(hooks), b''))

    for i in range(5):
        assert run(f'round-{i}'.encode() + b' abcde' * 140)[0]['status'] == 'completed'
    before = unpack(lib.dc_messages(handle, chat))
    config['desktop_context'] = dict(modelLimit=8192, deploymentLimit=0, selectedLimit=8192,
                                     outputLimit=512, requestedOutput=128, reserve=128,
                                     strategy='semantic', automatic=True, threshold=.4,
                                     retainTurns=2, summaryOutput=128)
    server.requests.clear()
    server.mode = scenario
    outcome, committed = run(b'verify')
    expected = 'cancelled' if scenario == 'hold' else 'failed'
    assert outcome['status'] == expected and committed, outcome
    records = unpack(lib.dc_workspace_request(handle, encode(dict(op='context.records', chat=scenario))))
    assert records['ok'] and records['data'][0]['status'] == expected
    assert 'context.compacting' in events and server.requests == ['summary']
    messages = unpack(lib.dc_messages(handle, chat))
    assert messages[:-1] == before and messages[-1]['content'] == 'verify'
    server.mode = 'ok'
    server.release.set()
    assert run(None)[0]['status'] == expected  # Resume of a settled turn remains a pure observation.
    count = len(server.requests)
    assert retry('f' * 64)['status'] != 'completed' and len(server.requests) == count
    assert retry(outcome['retryRun'])['status'] == 'completed'
    assert server.requests[count:] == ['summary', 'model']
    latest = unpack(lib.dc_messages(handle, chat))
    assert latest[:-1] == messages and latest[-1]['role'] == 'assistant'
    count = len(server.requests)
    assert retry(outcome['retryRun'])['status'] != 'completed' and len(server.requests) == count
    assert not reviews
    print(f'PASS {scenario}: original messages retained, authorized retry settled once, stale retry inert')


with ThreadingHTTPServer(('127.0.0.1', 0), Model) as server, tempfile.TemporaryDirectory() as temp:
    server.requests = []
    server.entered = threading.Event()
    server.release = threading.Event()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    work, state = Path(temp) / 'work', Path(temp) / 'private'
    work.mkdir()
    state.mkdir(mode=0o700)
    handle = lib.dc_open(str(state).encode())
    assert handle
    try:
        for scenario in ('fail', 'hold'):
            check(server, work, handle, scenario)
    finally:
        server.release.set()
        lib.dc_close(handle)
        server.shutdown()
        thread.join()
