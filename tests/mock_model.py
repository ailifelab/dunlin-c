"""Deterministic, loopback-only Chat Completions test server. No model or key required."""
import argparse
import json
import socket
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn


def wire(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def call(idx, name, args):
    return {"id": "id-{}".format(idx), "type": "function",
            "function": {"name": name, "arguments": json.dumps(args, ensure_ascii=False)}}


# One pixel PNG, independent of the live visual counting fixture.
req_png_url = ('data:image/png;base64,'
               'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8'
               '/x8AAwMCAO+jRZkAAAAASUVORK5CYII=')


def prompt_text(message):
    content = message['content']
    return content[0]['text'] if isinstance(content, list) else content


def repair_fixture(prompt, steps):
    variant = prompt[len('repair-'):]
    name = 'write_file' if variant == 'approval' else 'calc'
    valid = {'op':'mul','a':711,'b':23} if name == 'calc' else {'path':'created','text':'ok'}
    invalid = dict(valid, op='"mul"') if name == 'calc' else {'path':'created','text':7}
    bad = call('bad', name, invalid)
    expected = 'calc.op contains extra quotes' if name == 'calc' else 'write_file.text must be a string'
    if variant == 'json':
        bad['function']['arguments'] = '{"a":7,"a":8}'
        expected = 'valid JSON with unique keys'
    before = [call('first','echo',{'text':'already ran'})] if variant == 'batch' else []
    if not steps:
        return before + [bad]
    assert [v['id'] for v in steps[0]['tool_calls']] == [v['id'] for v in before + [bad]]
    feedback_index = 1 + len(before)
    if before:
        assert steps[1] == {'role':'tool','tool_call_id':'id-first','content':'already ran'}
    feedback = steps[feedback_index]
    assert feedback['role'] == 'tool' and feedback['tool_call_id'] == 'id-bad'
    assert expected in feedback['content'] and 'same tool once' in feedback['content']
    if len(steps) == feedback_index + 1:
        if variant == 'text': return '16353'
        if variant == 'switch': return [call('other','echo',{'text':'16353'})]
        if variant == 'reuse-id': return [call('bad',name,valid)]
        return [call('new',name,invalid if variant == 'twice' else valid)]
    assert len(steps) == feedback_index + 3
    assert steps[-2]['tool_calls'] == [call('new',name,valid)]
    assert steps[-1] == {'role':'tool','tool_call_id':'id-new','content':'16353'}
    return steps[-1]['content']


def completion(req):
    msgs = req["messages"]
    pos = max(i for i, msg in enumerate(msgs) if msg["role"] == "user")
    prompt = prompt_text(msgs[pos])
    if prompt in ('picture', 'recall-picture'):
        assert req['max_tokens'] == 64 and req['enable_thinking'] is False and req['temperature']==0
        parts = msgs[0]['content']
        assert parts[0] == {'type':'text','text':'picture'}
        assert parts[1]['type'] == 'image_url'
        assert parts[1]['image_url']['url'] == req_png_url
        if prompt == 'recall-picture': return 'image retained'

    steps = msgs[pos + 1:]
    if prompt == '帮我安装wget（回归前文）':
        return 'ready'
    if prompt.startswith('请继续上一轮的软件安装任务，目标软件是 node。'):
        if not steps:
            return [call('first-probe','shell',{'command':'printf probe'})]
        if len(steps)==2:
            if any('previous tool proposal' in str(m.get('content','')).lower() for m in msgs if m['role']=='system'):
                return [call('next-step','shell',{'command':'./node --version'})]
            return [call('repeated-probe','shell',{'command':'printf probe'})]
        assert len(steps)==4 and steps[-1]['content']=='exit_code=0\nv20.fixture\n',steps
        return 'verified'
    if prompt.startswith('请继续上一轮的软件安装任务，目标软件是 wget。'):
        if not steps:
            return [call('lookup','shell',{'command':'printf present'})]
        if len(steps)==2:
            if any('previous response omitted' in str(m.get('content','')).lower() for m in msgs if m['role']=='system'):
                return [call('verify','shell',{'command':'./wget --version'})]
            return 'now checking version'
        assert len(steps)==4 and steps[-1]['content']=='exit_code=0\nv1.fixture\n',steps
        return 'verified'
    if prompt == 'progress-model':
        time.sleep(6)
        return 'waited'
    if prompt == 'ui-probe-cycle':
        if any(m.get('content')=='exit_code=0\nprogressed' for m in steps): return 'progressed'
        if not steps or len(steps)==2: return [call('probe'+str(len(steps)),'shell',{'command':'command -v sh'})]
        if any('already succeeded' in str(m.get('content','')) for m in steps): return [call('continue','shell',{'command':'printf progressed; :'})]
        return 'progressed'
    if prompt == 'ui-stages':
        return [call('stages','shell',{'command':"printf 'phase-one\\n'; sleep 1; printf 'phase-two\\n' >&2; sleep 2; printf done > ui-finished"})] if not steps else 'finished'
    if prompt == 'progress-long':
        return [call('progress','shell',{'command':'sleep 11; printf observed'})] if not steps else 'observed'

    if prompt.startswith('command-fix-'):
        mode = prompt[len('command-fix-'):]
        if not steps:
            return [call('broken', 'shell', {'command':'"printf original"'})]
        assert steps[1]['tool_call_id'] == 'id-broken'
        assert 'exit_code=127' in steps[1]['content'] and 'printf original' in steps[1]['content']
        if len(steps) == 2:
            assert 'executed' in msgs[0]['content'] and 'side effects' in msgs[0]['content']
            assert [m['content'] for m in msgs if m['role']=='user']==['multiple',prompt]
            assert not any(m.get('tool_calls') for m in msgs[:pos])
            if mode == 'prose': return 'not installed'
            if mode == 'twice': return [call('fixed','shell',{'command':'"printf original"'})]
            return [call('fixed','shell',{'command':'printf observed'})]
        assert steps[-1]['content'] == 'exit_code=0\nobserved'
        assert [m['content'] for m in msgs if m['role']=='user']==['multiple',prompt]
        assert req.get('tools')
        if mode == 'extra': return [call('extra','shell',{'command':'printf unwanted'})]
        return 'observed'
    if prompt.startswith('repair-'):
        assert req['max_tokens'] == 96 and req['enable_thinking'] is False
        correcting = (msgs[0]['role'] == 'system')
        if correcting:
            name = 'write_file' if prompt == 'repair-approval' else 'calc'
            assert [entry['function']['name'] for entry in req['tools']] == [name]
            assert 'must call' in msgs[0]['content'] and steps[-1]['tool_call_id'] == 'id-bad'
            assert [m['content'] for m in msgs if m['role']=='user']==[prompt]
        else:
            assert not steps or steps[-1]['tool_call_id'] == 'id-new'
        return repair_fixture(prompt, steps)
    if 'next-step-context' in str(msgs[-1].get('content','')):
        assert msgs[-1]['role']=='system'
        assert prompt=='require-choice'
        assert [m['content'] for m in msgs if m['role']=='user']==['previous-topic','require-choice']
        assert 'tool_choice' not in req and msgs[-2]=={'role':'tool','tool_call_id':'id-0','content':'observed'}
        return 'observed'
    if prompt.startswith('omission-') and not steps:
        name = 'write_file' if prompt == 'omission-denied' else 'echo'
        assert req['tool_choice']['function']['name'] == name
        assert req['max_tokens'] == 96
        if len(req['tools']) == 1 and 'previous response' in msgs[0]['content']:
            assert sum(m.get('content') == prompt for m in msgs) == 1
            if prompt == 'omission-crash': time.sleep(3)
            args = {'path':'never','text':'x'} if name == 'write_file' else {'text':'observed'}
            return [call('fixed',name,args)]
        return 'fabricated observation'
    if prompt.startswith('exec-file-repair-'):
        if not steps:return [call('broken','shell',{'command':'sh repaired.sh 50'})]
        if len(steps)==2:return [call('fix','write_file',{'path':'repaired.sh','text':'printf \'%s\\n\' "$@"'})]
        if len(steps)==4:
            assert [t['function']['name'] for t in req['tools']]==['shell']
            if prompt.endswith('twice'):return [call('fix-again','write_file',{'path':'repaired.sh','text':'printf fake'})]
            return [call('rerun','shell',{'command':'sh repaired.sh 50'})]
        if len(steps)==6:
            assert steps[-1]['content']=='exit_code=0\n50\n'
            return [call('remaining','shell',{'command':'sh repaired.sh 25 123454321 12345678987654321'})]
        assert steps[-1]['content']=='exit_code=0\n25\n123454321\n12345678987654321\n'
        return 'four observed'
    if prompt=='template-override':
        assert req['chat_template_kwargs']=={'chat_template':'test-template'}
        return 'template forwarded'
    if prompt.startswith('proposal-'):
        if prompt=='proposal-window':
            assert len(msgs)<=8 and msgs[0]['role']=='system'
            steps=[m for m in steps if m['role']!='system']
        regenerated=any(m['role']=='system' and 'previous tool proposal' in str(m.get('content','')).lower() for m in msgs)
        if prompt in ('proposal-during-args','proposal-after-args'):
            if not steps:return [call('bad-save','write_file',{'path':'parameterized.sh','text':7})]
            if len(steps)==2:
                if prompt=='proposal-during-args' and not regenerated:return {'provider_reject':'invalid_tool_call'}
                assert len(req['tools'])==1 and req['tools'][0]['function']['name']=='write_file'
                return [call('fixed-save','write_file',{'path':'parameterized.sh','text':'printf \'%s\\n\' "$@"'})]
            if len(steps)==4:
                assert steps[-1]['content']=='written'
                if prompt=='proposal-after-args' and not regenerated:return {'provider_reject':'invalid_tool_call'}
                return [call('run','shell',{'command':'sh parameterized.sh 50 25 123454321 12345678987654321'})]
            assert steps[-1]['content']=='exit_code=0\n50\n25\n123454321\n12345678987654321\n'
            return 'observed four inputs'
        if not steps:
            return [call('save','write_file',{'path':'parameterized.sh','text':'printf \'%s\\n\' "$@"'})]
        if len(steps)==2:
            assert steps[-1]['content']=='written',steps
            if not regenerated or prompt=='proposal-twice':
                return {'provider_reject':'invalid_tool_call','http':400 if prompt=='proposal-http' else 200}
            if prompt=='proposal-crash':time.sleep(8)
            return [call('run','shell',{'command':'sh parameterized.sh 50 25 123454321 12345678987654321'})]
        assert len(steps)==4 and steps[-1]['content']=='exit_code=0\n50\n25\n123454321\n12345678987654321\n',steps
        return 'observed four inputs'
    if prompt=='quote-script':
        if not steps:return [call('script','write_file',{'path':'quote-script.sh','text':'printf 11'})]
        return 'saved'
    if prompt.startswith('quote-result'):
        missing=prompt.startswith('quote-result-missing')
        bad={'command':prompt.split(' ',1)[1]+' quote-script.sh' if missing else '"dunlin-runner quote-script.sh"'}
        if not steps:
            assert any(m.get('content')=='quote-script' for m in msgs[:pos])
            return [call('quoted-command','shell',bad)]
        if len(steps)==2:
            feedback=steps[-1]['content']
            if missing:
                assert 'exit_code=127' in feedback and 'CLI observed executable paths' in feedback,feedback
                candidate=feedback.split('CLI observed executable paths (not executed): ')[1].split('\n')[0].split(', ')[0]
            else:
                assert 'ONE executable' in feedback and 'outer quotes' in feedback,feedback
                candidate=feedback.split('Paths (not run): ')[1].split('. Correct the arguments')[0].split(', ')[0]
            assert candidate.endswith('/dunlin-runner3'),candidate
            return [call('fixed-command','shell',bad if prompt.endswith('twice') else {'command':candidate+' quote-script.sh'})]
        assert steps[-1]['content']=='exit_code=0\n11',steps
        return '11'
    if prompt == 'ui-date' and not steps:
        return [call('ui-date','shell',{'command':'date'})]
    if prompt.startswith('ui-write-') and not steps:
        name=prompt[len('ui-write-'):]
        return [call('ui-write','shell',{'command':"printf '%s' '"+name+"' > ui-"+name+".txt"})]
    if prompt == 'ui-filequoted':
        if not steps:return [call('quoted','write_file',{'path':'"ui-quoted.py"','text':'plain'})]
        if len(steps)==2:
            assert 'extra surrounding quote' in steps[-1]['content']
            return [call('plain','write_file',{'path':'ui-quoted.py','text':'plain'})]
        assert len(steps)==4 and steps[-1]['content']=='written',steps
        return 'file-saved'
    if prompt == 'ui-filewrite' and not steps:
        return [call('ui-filewrite','write_file',{'path':'ui-file.txt','text':'file'})]
    if prompt == 'ui-inline' and not steps:
        return [call('ui-inline','shell',{'command':"/bin/sh -c 'printf inline'"})]
    if prompt.startswith('ui-curl ') and not steps:
        return [call('ui-curl','shell',{'command':"curl -s '"+prompt[8:]+"'"})]
    if prompt == "forever":
        return [call(len(steps), "echo", {"text": "again"})]
    if steps:
        assert steps[0]["role"] == "assistant", steps
        issued = steps[0]["tool_calls"]
        assert [m["tool_call_id"] for m in steps[1:]] == [t["id"] for t in issued], steps
        assert all(m["role"] == "tool" for m in steps[1:])
        if prompt == "require-choice": assert "tool_choice" not in req
        return "results=" + "|".join(m["content"] for m in steps[1:])
    if prompt == 'picture': return [call(0,'calc',{'op':'mul','a':3,'b':17})]
    if prompt == "require-choice":
        assert req.get("tool_choice") == {"type":"function","function":{"name":"echo"}}
        return [call(0,"echo",{"text":"observed"})]
    if prompt == "remember":
        return "history=" + "/".join(m["content"] for m in msgs[:pos] if m["role"] == "user")
    if prompt == "inspect-history":
        return json.dumps(msgs[:pos], ensure_ascii=False)
    if prompt in ("single", "multiple", "duplicate"):
        calls = [call(0, "echo", {"text": "你好"})]
        if prompt != "single":
            calls.append(call(0 if prompt == "duplicate" else 1, "calc", {"a": 7, "op": "mul", "b": 6}))
        return calls
    if prompt == "unknown": return [call(0, "no_such_tool", {})]
    if prompt == "invalid_args": return [call(0, "calc", {"a": [], "op": "add", "b": 1})]
    if prompt == "bad_args_json":
        c = call(0, "echo", {})
        c["function"]["arguments"] = '{"text":"x","text":"y"}'
        return [c]
    if prompt.startswith("file "): return [call(0, "read_file", {"path": prompt[5:]})]
    if prompt == "write": return [call(0, "write_file", {"path": "created", "text": "approved"})]
    if prompt == "shell-error":
        return [call(0, "shell", {"command":"printf observed-error >&2; exit 201"})]
    if prompt == "exec": return [call(0, "exec", {"executable": "/bin/echo", "argv": ["literal;$(not-run)"]})]
    if prompt == "crash_tool":
        return [call(0, "exec", {"executable": "/bin/sh", "argv": ["-c", "echo launch >> marker; exec /bin/sleep 20"]})]
    if prompt == "large": return "x" * 32000
    if prompt == "outlimit": return [call(0, "echo", {"text": "z" * 16000})]
    return "reply:" + prompt


class MockHandler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        assert self.path == '/cli-read' and not self.headers.get('Authorization')
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'readonly response')

    def do_POST(self):
        try:
            self.handle_model()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def handle_model(self):
        assert self.path == "/v1/chat/completions", self.path
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])).decode("utf-8"))
        assert req["model"] == "mock-local"
        prompt = next(prompt_text(m) for m in reversed(req["messages"]) if m["role"] == "user")
        stream = req["stream"]
        assert isinstance(stream, bool)
        with self.server.received:
            self.server.prompts.append(prompt)
            self.server.received.notify_all()
        if self.server.bearer and self.headers.get("Authorization") != "Bearer " + self.server.bearer:
            self.send_error(401)
            return
        if prompt == "redirect":
            self.send_response(307)
            self.send_header("Location", "/must-not-follow")
            self.end_headers()
            return
        if prompt in ("rate", "unsupported"):
            self.send_response(429 if prompt == "rate" else 400)
            self.end_headers()
            self.wfile.write(b'{"error":"simulated-remote-sensitive-string"}')
            return
        if prompt in ("slow", "cancel", "kill_model"):
            time.sleep(self.server.wait_seconds)
        if prompt == 'repair-crash' and req['messages'][-1]['role'] == 'tool':
            time.sleep(self.server.wait_seconds)
        if prompt == "disconnect":
            self.connection.shutdown(socket.SHUT_RDWR)
            return
        reply = completion(req)
        if isinstance(reply,dict) and 'provider_reject' in reply:
            self.send_response(reply.get('http',200));self.end_headers()
            data=wire({'error':{'code':reply['provider_reject'],'message':'simulated-remote-sensitive-string'}})
            self.wfile.write(b'data: '+data+b'\n\n' if stream else data)
            return
        tools = isinstance(reply, list)
        reason = "length" if prompt=="output-length" else "tool_calls" if tools else "stop"
        # These fixtures intentionally propose an unadvertised tool. The client,
        # rather than a crashed server thread, must reject that proposal.
        if tools and prompt not in ("unknown","repair-switch","command-fix-extra","exec-file-repair-twice") and not (prompt=="multiple" and "tool_choice" in req):
            assert all(c["function"]["name"] in [t["function"]["name"] for t in req["tools"]] for c in reply)
        msg = {"role": "assistant", "content": None if tools else reply}
        if tools: msg["tool_calls"] = reply
        doc = {"choices": [{"index": 0, "message": msg, "finish_reason": reason}]}
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream" if stream else "application/json")
        self.end_headers()
        if not stream:
            self.wfile.write(b'{"choices":' if prompt == "malformed" else wire(doc))
            return
        if prompt == "malformed":
            self.wfile.write(b'data: {"choices":bad}\n\n')
            return
        self.wfile.write(b": model ping\r\n\r\n")
        deltas = [{"role": "assistant"}]
        if tools:
            first, second = [], []
            for i, c in enumerate(reply):
                fn = c["function"]
                n = max(1, len(fn["arguments"]) // 2)
                first.append({"index": i, "id": c["id"][:2], "type": "function",
                              "function": {"name": fn["name"][:1], "arguments": fn["arguments"][:n]}})
                second.append({"index": i, "id": c["id"][2:], "function": {
                    "name": fn["name"][1:], "arguments": fn["arguments"][n:]}})
            deltas.extend([{"tool_calls": first}, {"tool_calls": list(reversed(second))}])
        else:
            deltas.extend([{"content": reply[:3]}, {"content": reply[3:]}])
        deltas.append({})
        for i, delta in enumerate(deltas):
            entry = {"choices": [{"index": 0, "delta": delta,
                                  "finish_reason": reason if i == len(deltas)-1 else None}]}
            frame = b"data: " + wire(entry) + b"\r\n\r\n"
            for start in range(0, len(frame), 7):
                self.wfile.write(frame[start:start+7])
        if prompt != "truncated":
            self.wfile.write(b"data: [DONE]\n\n")


class MockServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, port=0, bearer=None):
        HTTPServer.__init__(self, ("127.0.0.1", port), MockHandler)
        self.bearer = bearer
        self.received = threading.Condition()
        self.prompts = []
        self.wait_seconds = 8
        self.errors = []
        self.workers = []

    def process_request(self, request, client_address):
        worker = threading.Thread(target=self.process_request_thread,
                                  args=(request, client_address), daemon=True)
        self.workers.append(worker)
        worker.start()

    def join_workers(self, timeout=10):
        deadline = time.monotonic() + timeout
        for worker in self.workers:
            worker.join(max(0, deadline - time.monotonic()))
        assert not any(worker.is_alive() for worker in self.workers), 'model fixture workers did not finish'
        self.assert_healthy()

    def server_close(self):
        HTTPServer.server_close(self)
        # Python 3.6 does not join ThreadingMixIn workers. Even on newer Python,
        # daemon workers can otherwise report a failure after the final check.
        self.join_workers()

    def handle_error(self, request, client_address):
        self.errors.append(traceback.format_exc())

    def assert_healthy(self):
        assert not self.errors, 'model fixture worker failed:\n' + '\n'.join(self.errors)


if __name__ == "__main__":
    args = argparse.ArgumentParser()
    args.add_argument("--port", default=8089, type=int)
    server = MockServer(args.parse_args().port)
    print("http://127.0.0.1:{}/v1".format(server.server_port), flush=True)
    server.serve_forever()
