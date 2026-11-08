#!/usr/bin/env python3
"""Isolated CLI/model-profile acceptance; only the loopback provider is mocked."""
import argparse
import http.server
import json
import os
import pathlib
import pty
import re
import select
import subprocess
import tempfile
import termios
import threading
import time
import traceback


class Provider(http.server.HTTPServer):
    def __init__(self):
        super().__init__(('127.0.0.1', 0), Handler)
        self.requests = []
        self.errors = []

    def handle_error(self, _request, _address):
        self.errors.append(traceback.format_exc())


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.requests.append((self.path, request))
        responses = self.path.endswith('/responses')
        messages = request['input' if responses else 'messages']
        users = [m['content'] for m in messages if m.get('role') == 'user']
        users = [''.join(p.get('text', '') for p in u) if isinstance(u, list) else u for u in users]
        prompt = users[-1]
        model = request['model']
        text = '{}|{}|history={}'.format(model, prompt, ','.join(users[:-1]))
        message = {'role': 'assistant', 'content': text}
        if prompt != 'quiet':
            message['reasoning_content'] = ('Provider reasoning: 中文🙂.' if prompt != 'controls'
                                            else 'Provider reasoning: \x1b[2J\u202eCTRL')
        finish = 'stop'
        if prompt == 'use-echo':
            if messages[-1]['role'] == 'tool':
                assert messages[-1]['content'] == 'observed-after-switch', messages[-1]
                assert messages[-1]['tool_call_id'] == 'fixture-echo', messages[-1]
                message['content'] = 'Tool result: observed-after-switch'
            else:
                message.update(content=None, tool_calls=[{'id': 'fixture-echo', 'type': 'function',
                               'function': {'name': 'echo', 'arguments': '{"text":"observed-after-switch"}'}}])
                finish = 'tool_calls'
        response = {'choices': [{'index': 0, 'message': message, 'finish_reason': finish}]}
        if responses:
            response = {'id': 'response-fixture', 'status': 'completed', 'output': [
                {'type': 'reasoning', 'id': 'reasoning-fixture',
                 'summary': [{'type': 'summary_text', 'text': 'Provider summary: short and public.'}],
                 'encrypted_content': 'opaque-never-display'},
                {'type': 'message', 'id': 'message-fixture', 'role': 'assistant', 'status': 'completed',
                 'content': [{'type': 'output_text', 'text': text}]}]}
        if request.get('stream') and not responses:
            parts = []
            if 'reasoning_content' in message:
                for part in ('Provider reasoning: ', '中文🙂.'):
                    parts.append({'choices': [{'index': 0, 'delta': {'reasoning_content': part}, 'finish_reason': None}]})
            parts.append({'choices': [{'index': 0, 'delta': {'content': text}, 'finish_reason': None}]})
            parts.append({'choices': [{'index': 0, 'delta': {}, 'finish_reason': 'stop'}]})
            wire = b''.join(b'data: ' + json.dumps(p, ensure_ascii=False).encode() + b'\n\n' for p in parts)
            wire += b'data: [DONE]\n\n'
        else:
            wire = json.dumps(response, ensure_ascii=False).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream' if request.get('stream') else 'application/json')
        self.send_header('Content-Length', str(len(wire)))
        self.end_headers()
        self.wfile.write(wire)


class Terminal:
    def __init__(self, case, *options):
        self.master, self.slave = pty.openpty()
        self.before = termios.tcgetattr(self.slave)
        self.process = subprocess.Popen(case.command(*options, 'chat'), stdin=self.slave,
                                        stderr=self.slave, stdout=subprocess.PIPE,
                                        env=case.env)
        self.transcript = bytearray()
        self.read_prompt()

    def read_prompt(self):
        captured = bytearray()
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if select.select([self.master], [], [], .05)[0]:
                data = os.read(self.master, 65536)
                captured.extend(data)
                self.transcript.extend(data)
                # A quoted '> ' in provider text is not a new editor prompt.
                if re.search(rb'(?:^|[\r\n])> $', captured):
                    return bytes(captured)
            if self.process.poll() is not None:
                raise AssertionError(('CLI stopped before prompt', self.process.returncode, captured))
        raise AssertionError(('CLI prompt timeout', captured))

    def send(self, line):
        os.write(self.master, line.encode() + b'\n')
        return self.read_prompt()

    def finish(self):
        os.write(self.master, b'/q\n')
        deadline = time.monotonic() + 8
        while self.process.poll() is None and time.monotonic() < deadline:
            if select.select([self.master], [], [], .05)[0]:
                self.transcript.extend(os.read(self.master, 65536))
        out, _ = self.process.communicate(timeout=2)
        assert self.process.returncode == 0, (self.process.returncode, out, self.transcript)
        assert termios.tcgetattr(self.slave) == self.before, 'terminal attributes not restored'
        return out

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        os.close(self.master)
        os.close(self.slave)


class Case:
    def __init__(self, binary, root, provider):
        self.binary = os.path.abspath(binary)
        self.root = pathlib.Path(root)
        self.provider = provider
        self.cfg = self.root / 'config.json'
        for name in ('work', 'sessions', 'home'):
            (self.root / name).mkdir(mode=0o700)
        self.env = dict(os.environ, HOME=str(self.root / 'home'), TERM='xterm-256color',
                        LC_ALL='en_US.UTF-8', NO_PROXY='*', no_proxy='*',
                        DUNLIN_SHELL_FIXTURE_KEY='fixture-never-display')
        self.base = dict(base_url='http://127.0.0.1:{}/v1'.format(provider.server_port),
                         model='fixture-alpha', stream=False, workspace='work',
                         session_dir='sessions', key_env='DUNLIN_SHELL_FIXTURE_KEY', timeout_ms=2000)
        self.configure()

    def configure(self, **values):
        config = dict(self.base, **values)
        if 'models' in values:
            for name in ('base_url', 'model', 'key_env'):
                del config[name]
        self.cfg.write_text(json.dumps(config))
        self.cfg.chmod(0o600)

    def profiles(self, **values):
        common = dict(base_url=self.base['base_url'], key_env=self.base['key_env'])
        self.configure(models={
            'alpha': dict(common, model='fixture-alpha', protocol='chat',
                          reasoning_capabilities=['none', 'levels']),
            'beta': dict(common, model='fixture-beta', protocol='responses',
                         reasoning_capabilities=['none', 'levels']),
            'gamma': dict(common, model='fixture-gamma', protocol='chat')},
            active_model='alpha', **values)

    def command(self, *args):
        return [self.binary, '--config', str(self.cfg), '--permissions', 'strict'] + list(args)

    def run(self, *args, **kwargs):
        result = subprocess.run(self.command(*args), env=self.env, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=8, **kwargs)
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
        assert b'fixture-never-display' not in result.stdout + result.stderr
        assert b'opaque-never-display' not in result.stdout + result.stderr
        return result


def reasoning_reproduction(case):
    terminal = Terminal(case, '--session', 'reasoning-repro')
    try:
        seen = terminal.send('hello')
        output = terminal.finish()
        assert output == b'fixture-alpha|hello|history=\n', output
        assert b'Provider reasoning:' in seen, ('provider returned reasoning but shell omitted it', seen)
    finally:
        terminal.close()
    print('PASS default natural answer and actual provider reasoning')


def reasoning_controls(case):
    for stream in (False, True):
        case.configure(stream=stream)
        terminal = Terminal(case, '--session', 'reasoning-' + str(stream))
        try:
            assert b'Provider reasoning:' in terminal.send('visible')
            assert b'Reasoning display: off' in terminal.send('/reasoning off')
            assert b'Provider reasoning:' not in terminal.send('hidden')
            assert b'Reasoning display: on' in terminal.send('/reasoning on')
            quiet = terminal.send('quiet')
            assert b'[Reasoning' not in quiet, quiet
            output = terminal.finish()
            assert b'fixture-alpha|visible|' in output and b'fixture-alpha|hidden|' in output
            assert len(output.splitlines()) == 3, output
        finally:
            terminal.close()
    case.configure(show_reasoning=False)
    terminal = Terminal(case)
    try:
        assert b'Provider reasoning:' not in terminal.send('configured-hidden')
        terminal.finish()
    finally:
        terminal.close()
    case.configure()
    terminal = Terminal(case)
    try:
        escaped = terminal.send('controls')
        assert b'\\x1b[2J\\u202eCTRL' in escaped and b'\x1b[2J' not in escaped, escaped
        terminal.finish()
    finally:
        terminal.close()
    print('PASS reasoning in JSON and SSE, toggle/config suppression, absent reasoning and terminal escaping')


def machine_modes(case):
    case.configure()
    plain = case.run('--json', '--session', 'json-mode', 'run', 'machine')
    row = json.loads(plain.stdout)
    assert set(row) == {'status', 'session', 'text'} and row['status'] == 'completed', row
    assert b'Provider reasoning:' not in plain.stderr
    events = case.run('--events', 'run', 'machine')
    rows = [json.loads(line) for line in events.stdout.splitlines()]
    assert rows[-1]['status'] == 'completed'
    assert any(r.get('event') == 'reasoning.delta' and 'Provider reasoning:' in r['data'] for r in rows)
    assert all(set(r) == {'event', 'data'} for r in rows[:-1]), rows
    piped = case.run('--json', 'chat', input=b'/model beta\n/reasoning off\n')
    rows = [json.loads(line) for line in piped.stdout.splitlines()]
    assert len(rows) == 2 and '|/model beta|' in rows[0]['text'] and '|/reasoning off|' in rows[1]['text']
    print('PASS explicit JSON/events schema and piped slash commands remain model input')


def profile_workflow(case):
    case.profiles()
    config_before = case.cfg.read_bytes()
    files_before = {p.name: p.read_bytes() for p in (case.root / 'sessions').iterdir() if p.is_file()}
    terminal = Terminal(case, '--session', 'conversation')
    try:
        before = len(case.provider.requests)
        inventory = terminal.send('/models')
        assert all(label in inventory for label in (b'alpha', b'beta', b'gamma', b'chat', b'responses'))
        assert b'127.0.0.1' not in inventory and b'DUNLIN_SHELL_FIXTURE_KEY' not in inventory
        assert b'fixture-never-display' not in inventory
        assert b'Reasoning effort: default' in terminal.send('/effort')
        assert b'Reasoning effort: low' in terminal.send('/effort low')
        assert b'Reasoning effort: low' in terminal.send('/effort invalid')
        terminal.send('/model missing')
        assert b'Model: alpha' in terminal.send('/model')
        assert b'Model: alpha' in terminal.send('/model status')
        assert len(case.provider.requests) == before
        assert files_before == {p.name: p.read_bytes() for p in (case.root / 'sessions').iterdir() if p.is_file()}
        terminal.send('alpha-first')
        assert case.provider.requests[-1][1]['reasoning_effort'] == 'low'
        terminal.send('/model beta')
        summary = terminal.send('beta-first')
        assert b'Provider summary:' in summary and b'[Reasoning summary]' not in summary
        assert b'opaque-never-display' not in summary
        terminal.send('/model alpha')
        terminal.send('alpha-second')
        terminal.send('/model gamma')
        rejected = terminal.send('/effort high')
        assert b'Reasoning effort: default' in rejected and b'support' in rejected.lower(), rejected
        terminal.send('gamma-first')
        terminal.send('use-echo')
        terminal.send('/model alpha')
        assert b'Reasoning effort: low' in terminal.send('/effort')
        output = terminal.finish()
        assert b'fixture-alpha|alpha-first|history=\n' in output, output
        assert b'fixture-beta|beta-first|history=\n' in output, output
        assert b'fixture-alpha|alpha-second|history=alpha-first\n' in output, output
        assert b'fixture-gamma|gamma-first|history=\n' in output, output
        assert b'Tool result: observed-after-switch\n' in output, output
    finally:
        terminal.close()
    assert case.cfg.read_bytes() == config_before, 'shell changes must not rewrite config'
    requests_before = len(case.provider.requests)
    rejected = subprocess.run(case.command('--model', 'missing', '--json', 'run', 'unused'),
                              env=case.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=8)
    assert rejected.returncode == 64 and json.loads(rejected.stdout)['status'] == 'error', rejected.stdout
    assert len(case.provider.requests) == requests_before
    inventory = json.loads(case.run('--model', 'alpha', '--json', 'sessions').stdout)
    assert 'conversation' in inventory, ('session inventory must expose reusable public IDs', inventory)
    listed_id = next(name for name in inventory if name == 'conversation')
    resumed = case.run('--model', 'alpha', '--session', listed_id, '--json', 'run', 'after-restart')
    result = json.loads(resumed.stdout)
    assert result['session'] == 'conversation', result
    assert result['text'] == 'fixture-alpha|after-restart|history=alpha-first,alpha-second', result
    beta = json.loads(case.run('--model', 'beta', '--session', result['session'], '--json', 'run', 'beta-restart').stdout)
    assert beta['text'] == 'fixture-beta|beta-restart|history=beta-first', beta
    # The old single-model journal remains untouched when named models are enabled.
    legacy = case.root / 'sessions' / 'reasoning-repro.json'
    original = legacy.read_bytes()
    isolated = json.loads(case.run('--model', 'alpha', '--session', 'reasoning-repro', '--json', 'run', 'isolated').stdout)
    assert isolated['text'] == 'fixture-alpha|isolated|history=' and legacy.read_bytes() == original
    print('PASS model aliases, idle switching, capability errors, adapter lifetime, isolated histories and restart')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary')
    parser.add_argument('--reasoning-repro', action='store_true')
    args = parser.parse_args()
    provider = Provider()
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='dunlin-shell-profiles-') as root:
            case = Case(args.binary, root, provider)
            reasoning_reproduction(case)
            if not args.reasoning_repro:
                reasoning_controls(case)
                machine_modes(case)
                profile_workflow(case)
    finally:
        provider.shutdown()
        provider.server_close()
        thread.join(timeout=2)
        assert not provider.errors, provider.errors


if __name__ == '__main__':
    main()
