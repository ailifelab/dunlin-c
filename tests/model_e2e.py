"""Exercise the shipped CLI and durable store across separate processes, using only loopback."""
import json
import os
import pty
import re
import select
import ssl
import signal
import subprocess
import sys
import tempfile
import threading
import termios
import time
from pathlib import Path

from mock_model import MockServer, req_png_url
import base64


class Case:
    def __init__(self, exe, directory, server):
        self.exe = os.path.abspath(exe)
        self.root = Path(directory)
        self.server = server
        self.cfg = self.root / 'config.json'
        self.work = self.root / 'work'
        self.work.mkdir()
        self.env = dict(os.environ, DUNLIN_TEST_BEARER=server.bearer)
        self.env['no_proxy'] = self.env['NO_PROXY'] = '*'
        self.server_url = 'http://127.0.0.1:{}/v1'.format(server.server_port)
        self.configure()

    def configure(self, **kwargs):
        cfg = dict(base_url=self.server_url, model='mock-local', stream=False, workspace='work',
                   session_dir='sessions', key_env='DUNLIN_TEST_BEARER', timeout_ms=10000)
        cfg.update(kwargs)
        self.cfg.write_text(json.dumps(cfg))

    def cmd(self, args, session=None, events=False):
        cmd = [self.exe, '--config', str(self.cfg), '--permissions', getattr(self,'permission_mode','strict'), '--events' if events else '--json']
        if session: cmd += ['--session', session]
        return cmd + args

    def execute(self, args, code=0, session=None, events=False, stdin=None):
        cmd = self.cmd(args, session, events)
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, input=stdin,
                              env=self.env, timeout=15)
        self.server.assert_healthy()
        assert proc.returncode == code, (cmd, proc.returncode, proc.stdout, proc.stderr)
        assert self.server.bearer.encode() not in proc.stdout + proc.stderr
        assert b'simulated-remote-sensitive-string' not in proc.stdout + proc.stderr
        rows = [json.loads(line.decode('utf-8')) for line in proc.stdout.splitlines()]
        if events: return rows
        assert len(rows) == 1, rows
        return rows[0]

    def run(self, prompt, code=0, **kwargs):
        return self.execute(['run', prompt], code, **kwargs)



def finish_terminal(process, master, timeout=15):
    # A terminal consumer must keep draining rendered output while the editor
    # restores TCSADRAIN mode. stdout remains independently captured JSON.
    stop = threading.Event()
    captured=bytearray()
    def drain():
        while not stop.is_set():
            if select.select([master], [], [], 0.05)[0]:
                try:
                    chunk=os.read(master,8192)
                    if not chunk:return
                    captured.extend(chunk)
                except OSError:
                    return
    reader = threading.Thread(target=drain, daemon=True)
    reader.start()
    try:
        output,_=process.communicate(timeout=timeout)
        return output,captured
    finally:
        stop.set()
        reader.join(timeout=1)


def basics(c):
    assert c.execute(['version'])
    assert c.execute(['validate'])['status'] == 'valid'
    assert c.execute(['doctor'])['model_configured']
    assert c.execute(['plugins'])
    assert {t['function']['name'] for t in c.execute(['tools'])} == {
        'echo', 'calc', 'read_file', 'write_file', 'exec', 'http_get'}
    for stream in [False, True]:
        c.configure(stream=stream)
        for prompt, reply in [('single', 'results=你好'), ('multiple', 'results=你好|42'),
                              ('你好', 'reply:你好')]:
            assert c.run(prompt)['text'] == reply
    rows = c.run('你好', events=True)
    assert rows[-1]['text'] == 'reply:你好'
    deltas = [r['data'] for r in rows[:-1] if r.get('event') == 'delta']
    assert ''.join(deltas) == 'reply:你好', rows
    session = rows[-1]['session']
    assert c.execute(['resume', 'remember'], session=session)['text'] == 'history=你好'
    assert session in c.execute(['sessions'])
    checkpoint = (c.root / 'sessions' / (session + '.json')).read_text()
    assert 'event":"delta' not in checkpoint and c.server.bearer not in checkpoint
    print('PASS inventory, both protocols, SSE deltas, multiple ID-paired tool results, restart + multi-turn')


def turn_boundary(c):
    c.configure()
    session = 'turn-boundary'
    checkpoint = c.root / 'sessions' / (session + '.json')
    assert c.run('single', session=session)['text'] == 'results=你好'
    completed = json.loads(checkpoint.read_text())['messages']
    assert [m['role'] for m in completed] == ['user', 'assistant', 'tool', 'assistant']
    assert c.run('output-length', 4, session=session)['status'] == 'budget'
    assert c.run('write', 3, session=session)['status'] == 'denied'
    c.configure(max_rounds=1)
    partial = c.run('multiple', 4, session=session, events=True)
    assert [r['data'] for r in partial if r.get('event') == 'tool.result'] == ['id-0', 'id-1']
    stopped = json.loads(checkpoint.read_text())
    audit = stopped['messages']
    assert audit[-1]['role'] == 'tool'
    requests = len(c.server.prompts)
    assert c.execute(['resume'], 4, session=session)['status'] == 'budget'
    assert len(c.server.prompts) == requests
    # v1 checkpoint immediately after both results were saved, before the next model step.
    resume_path = checkpoint.with_name('boundary-active.json')
    resume_path.write_text(json.dumps(dict(stopped, active=True, rounds=1)))
    resume_path.chmod(0o600)
    c.configure(max_messages=9)
    assert c.run('new input', 6, session='boundary-active')['status'] == 'busy'
    resumed = c.execute(['resume'], session='boundary-active', events=True)
    assert resumed[-1]['text'] == 'results=你好|42'
    assert not any(r.get('event') == 'tool.start' for r in resumed)
    c.configure()
    received = json.loads(c.run('inspect-history', session=session)['text'])
    assert received == audit, received
    assert json.loads(checkpoint.read_text())['messages'][:-2] == audit
    # A small budget evicts closed old turns while preserving the new exchange.
    c.configure(max_messages=len(completed)+4)
    bounded = c.run('another subject', session=session, events=True)
    assert bounded[-1]['text'] == 'reply:another subject'
    assert not any(r.get('event','').startswith('tool.') for r in bounded)
    c.configure()
    print('PASS follow-up sees successful, failed and denied observations; bounded history evicts complete old turns without replay')


def command_repair(c):
    for mode,status,approvals in [('ok','completed',2),('prose','failed',1),
                                  ('twice','failed',2),('deny','denied',2),('extra','denied',3)]:
        prompt='command-fix-'+mode
        c.configure(enabled_tools=['echo','calc','shell'])
        assert c.run('multiple',session=prompt)['text']=='results=你好|42'
        saved=json.loads((c.root/'sessions'/(prompt+'.json')).read_text())['messages']
        c.configure(enabled_tools=['shell'],max_tokens=256,enable_thinking=False)
        master,slave=pty.openpty()
        proc=subprocess.Popen(c.cmd(['chat'],session=prompt,events=True),
                              stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=c.env)
        os.close(slave)
        try:
            read_until(master,b'> ')
            os.write(master,prompt.encode()+b'\n')
            for index in range(approvals):
                review=read_until(master,b'Type yes to allow once: ')
                assert b'printf' in review
                os.write(master,b'no\n' if (mode=='deny' and index==1) or (mode=='extra' and index==2) else b'yes\n')
            read_until(master,b'> ')
            os.write(master,b'\x04')
            out,_=finish_terminal(proc,master,timeout=15)
            rows=[json.loads(line) for line in out.splitlines()]
            assert rows[-1]['status']==status,rows
            c.server.assert_healthy()
            assert sum(r.get('event')=='tool.start' for r in rows)==(1 if mode in ('prose','deny') else 2),rows
            state=json.loads((c.root/'sessions'/(prompt+'.json')).read_text())
            assert state['messages'][:len(saved)]==saved
            assert 'exit_code=127' in state['messages'][len(saved)+2]['content'], state
            if mode=='prose':
                assert rows[-1]['text']=='not installed'
            if mode=='twice':
                assert 'Initial command failure:' in rows[-1]['text'], rows
                assert 'exit_code=127' in rows[-1]['text'], rows
                assert 'Correction command failure:' in rows[-1]['text'], rows
            if mode=='ok': assert rows[-1]['text']=='observed'
            else: assert not any(r.get('event')=='delta' for r in rows),rows
        finally:
            if proc.poll() is None: proc.kill();proc.wait()
            os.close(master)
    c.configure()
    print('PASS generic command correction, exact error feedback, two approvals, text rejected, second exit failure and correction denied')


def command_recovery(c):
    c.configure(enabled_tools=['shell'],max_tokens=256,enable_thinking=False)
    original=json.loads((c.root/'sessions/command-fix-ok.json').read_text())
    # Atomic checkpoint at corrected proposal commit, before approval/execution.
    staged=dict(original,active=True,messages=original['messages'][:-2])
    assert staged['messages'][-1]['tool_calls'][0]['id']=='id-fixed'
    for name in ['command-resume','command-resume-denied']:
        p=c.root/'sessions'/(name+'.json')
        p.write_text(json.dumps(staged))
        p.chmod(0o600)
    before=len(c.server.prompts)
    assert c.execute(['resume'],3,session='command-resume-denied')['status']=='denied'
    assert len(c.server.prompts)==before
    master,slave=pty.openpty()
    proc=subprocess.Popen(c.cmd(['resume'],session='command-resume',events=True),
                          stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=c.env)
    os.close(slave)
    try:
        review=read_until(master,b'Type yes to allow once: ')
        assert b'printf observed' in review and b'original' not in review
        os.write(master,b'yes\n')
        out,_=finish_terminal(proc,master,timeout=15)
        rows=[json.loads(line) for line in out.splitlines()]
        assert proc.returncode==0 and rows[-1]['text']=='observed',rows
        assert [r['data'] for r in rows if r.get('event')=='tool.start']==['id-fixed']
        assert len(c.server.prompts)==before+1
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        os.close(master)
    c.configure()
    assert c.run('remember',session='command-resume')['text']=='history=multiple/command-fix-ok'
    print('PASS staged execution correction recovery, TTY exact approval, piped resume denied, only unexecuted ID dispatched and completed memory retained')


def fresh_sessions(c):
    c.configure()
    first=subprocess.Popen(c.cmd(['chat']),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=c.env)
    second=subprocess.Popen(c.cmd(['chat']),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=c.env)
    try:
        assert second.poll() is None
        out,err=first.communicate(b'fresh-alpha\n',timeout=15)
        assert first.returncode==0,(out,err)
        a=json.loads(out)
        assert second.poll() is None
        out,err=second.communicate(b'fresh-beta\n',timeout=15)
        assert second.returncode==0,(out,err)
        b=json.loads(out)
        assert a['session']!=b['session']
        assert c.execute(['resume','remember'],session=a['session'])['text']=='history=fresh-alpha'
        fresh=c.run('remember')
        assert fresh['session'] not in (a['session'],b['session']) and fresh['text']=='history='
        state=json.loads((c.root/'sessions'/(b['session']+'.json')).read_text())
        assert [m['content'] for m in state['messages'] if m['role']=='user']==['fresh-beta']
    finally:
        for process in [first,second]:
            if process.poll() is None:process.kill();process.wait()
    print('PASS concurrent fresh CLI processes use distinct isolated IDs, each later launch fresh, explicit ID alone continues history')



def cli_frontend(c):
    c.permission_mode='read-only'
    c.configure(enabled_tools=['shell','exec','write_file'],max_tokens=256,enable_thinking=False)
    assert 'exit_code=0' in c.run('ui-date')['text']
    assert 'literal;$(not-run)' in c.run('exec')['text']
    # Auto GET must not load user curl configuration and write its output file.
    home=c.root/'cli-home';home.mkdir()
    marker=c.root/'curl-output'
    (home/'.curlrc').write_text('output = "'+str(marker)+'"\n')
    previous_home=c.env.get('HOME')
    c.env['HOME']=str(home)
    assert c.run('ui-curl '+c.server_url.replace('/v1','/cli-read'))['text']=='results=exit_code=0\nreadonly response'
    assert not marker.exists()
    if previous_home is None:c.env.pop('HOME',None)
    else:c.env['HOME']=previous_home
    master,slave=pty.openpty()
    proc=subprocess.Popen(c.cmd(['chat'],'ui-chat'),stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=c.env)
    os.close(slave)
    try:
        read_until(master,b'> ')
        count=len(c.server.prompts)
        for local in ['/help','/pwd','/files','/last','/permissions']:
            os.write(master,local.encode()+b'\n')
            output=read_until(master,b'> ')
            if local=='/help':assert b'--approval-default' in output
            if local=='/pwd':assert str(c.work.resolve()).encode() in output
        assert len(c.server.prompts)==count
        os.write(master,b'ui-write-denied\n')
        review=read_until(master,b'Type yes to allow once: ')
        assert b'yes/No' in review and b'ui-denied.txt' in review
        os.write(master,b'\n')
        read_until(master,b'> ')
        assert not (c.work/'ui-denied.txt').exists()
        for name,answer in [('one',b'y\n'),('two',b'a\n')]:
            os.write(master,('ui-write-'+name+'\n').encode())
            review=read_until(master,b'Type yes to allow once: ')
            assert ('ui-'+name+'.txt').encode() in review
            os.write(master,answer)
            read_until(master,b'> ')
            assert (c.work/('ui-'+name+'.txt')).read_text()==name
        os.write(master,b'ui-write-three\n')
        output=read_until(master,b'> ')
        assert b'Type yes' not in output and (c.work/'ui-three.txt').read_text()=='three'
        os.write(master,b'/files\n')
        listing=read_until(master,b'> ')
        assert str(c.work.resolve()/'ui-three.txt').encode() in listing, listing
        os.write(master,b'/last\n')
        assert b'ui-three.txt' in read_until(master,b'> ')
        os.write(master,b'ui-filewrite\n')
        assert b'write_file' in read_until(master,b'Type yes to allow once: ')
        os.write(master,b'n\n')
        read_until(master,b'> ')
        assert not (c.work/'ui-file.txt').exists()
        os.write(master,b'/permissions reset\n')
        read_until(master,b'> ')
        os.write(master,b'ui-write-four\n')
        read_until(master,b'Type yes to allow once: ')
        os.write(master,b'N\n')
        read_until(master,b'> ')
        assert not (c.work/'ui-four.txt').exists()
        os.write(master,b'/permissions yes\n')
        assert b'Yes/no' in read_until(master,b'> ')
        os.write(master,b'ui-write-five\n')
        assert b'Yes/no' in read_until(master,b'Type yes to allow once: ')
        os.write(master,b'\n')
        read_until(master,b'> ')
        assert (c.work/'ui-five.txt').read_text()=='five'
        os.write(master,b'ui-filequoted\n')
        review=read_until(master,b'Type yes to allow once: ')
        assert b'ui-quoted.py' in review and b'\\\"ui-quoted' not in review,review
        os.write(master,b'y\n')
        read_until(master,b'> ')
        assert (c.work/'ui-quoted.py').read_text()=='plain'
        assert not (c.work/'"ui-quoted.py"').exists()
        os.write(master,b'/q\n')
        out,diagnostics=finish_terminal(proc,master)
        assert proc.returncode==0,(proc.returncode,bytes(diagnostics))
        rows=[json.loads(line) for line in out.splitlines()]
        assert [r['status'] for r in rows]==['denied','completed','completed','completed','denied','denied','completed','completed'],rows
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        os.close(master)
    assert c.run('ui-write-six',3,session='ui-chat')['status']=='denied'
    assert not (c.work/'ui-six.txt').exists()
    for args in [['help'],['--help'],['-h']]:
        assert 'Chat commands' in c.execute(args)['help']
    c.permission_mode='strict';c.configure()
    print('PASS CLI local help/workspace/files/last, readonly commands and GET without curlrc, y/n/a grants per tool, reset, defaults and restart isolation')


def quoted_command_followup(c):
    c.permission_mode='read-only'
    c.configure(enabled_tools=['shell','write_file'],max_tokens=256,enable_thinking=False)
    candidate=c.work/'dunlin-runner3';candidate.symlink_to('/bin/sh')
    env=dict(c.env,PATH=str(c.work.resolve())+os.pathsep+c.env.get('PATH',''))
    for name,answer,status in [('quote-result',b'y\n','completed'),('quote-result-denied',b'n\n','denied'),('quote-result-twice',None,'failed'),('quote-result-missing',b'y\n','completed'),('quote-result-missing-denied',b'n\n','denied')]:
        master,slave=pty.openpty()
        proc=subprocess.Popen(c.cmd(['chat'],name,True),stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=env)
        os.close(slave)
        try:
            read_until(master,b'> ');os.write(master,b'quote-script\n')
            assert b'quote-script.sh' in read_until(master,b'Type yes to allow once: ')
            os.write(master,b'y\n');read_until(master,b'> ')
            assert (c.work/'quote-script.sh').read_text()=='printf 11'
            request=name+' '+str(c.work.absolute()/'dunlin-runner') if name.startswith('quote-result-missing') else name
            os.write(master,request.encode()+b'\n')
            if name.startswith('quote-result-missing'):
                original=read_until(master,b'Type yes to allow once: ')
                assert str(c.work.absolute()/'dunlin-runner').encode() in original,original
                os.write(master,b'y\n')
            if answer:
                review=read_until(master,b'Type yes to allow once: ')
                assert str(candidate.absolute()).encode() in review and b'\\\"dunlin-runner ' not in review,review
                os.write(master,answer)
            read_until(master,b'> ');os.write(master,b'/q\n')
            out,diagnostics=finish_terminal(proc,master)
            rows=[json.loads(line) for line in out.splitlines()]
            assert proc.returncode==0,(proc.returncode,bytes(diagnostics))
            assert [r['status'] for r in rows if 'status' in r]==['completed',status],rows
            starts=[r['data'] for r in rows if r.get('event')=='tool.start']
            expected=['id-script']+(['id-quoted-command'] if name.startswith('quote-result-missing') else [])+(['id-fixed-command'] if status=='completed' else [])
            assert starts==expected,rows
            assert sum(r.get('event')=='tool.invalid_arguments' for r in rows)==(0 if name.startswith('quote-result-missing') else 1)
            assert sum(r.get('event')=='tool.execution_failed' for r in rows)==(1 if name.startswith('quote-result-missing') else 0)
            if status=='completed':assert rows[-1]['text']=='11'
        finally:
            if proc.poll() is None:proc.kill();proc.wait()
            os.close(master)
    candidate.unlink();c.permission_mode='strict';c.configure()
    print('PASS saved script -> natural follow-up, one generic quoted-command repair with observed version candidate, fresh ID, exact approval, denial and repeated malformed no-execution')


def proposal_retry(c):
    c.configure(omlx_chat_template='test-template')
    assert c.run('template-override')['text']=='template forwarded'
    c.configure(enabled_tools=['write_file','shell'],stream=True,max_tokens=256,enable_thinking=False)
    for name,status,answer in [('proposal-during-args','completed',b'y\n'),('proposal-after-args','completed',b'y\n'),('proposal-ok','completed',b'y\n'),('proposal-denied','denied',b'n\n'),('proposal-twice','failed',None),('proposal-http','failed',None)]:
        master,slave=pty.openpty();proc=subprocess.Popen(c.cmd(['chat'],name,True),stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=c.env);os.close(slave)
        try:
            read_until(master,b'> ');os.write(master,name.encode()+b'\n')
            read_until(master,b'Type yes to allow once: ');os.write(master,b'y\n')
            if answer:
                review=read_until(master,b'Type yes to allow once: ')
                assert b'12345678987654321' in review and b'parameterized.sh' in review
                os.write(master,answer)
            read_until(master,b'> ')
            # Success must have asked for the remaining shell call before the prompt.
            # Captured stdout verifies no earlier write or phantom call was replayed.
            os.write(master,b'/q\n');out,diagnostics=finish_terminal(proc,master)
            rows=[json.loads(line) for line in out.splitlines()]
            assert rows[-1]['status']==status,rows
            starts=[r['data'] for r in rows if r.get('event')=='tool.start']
            assert starts==(['id-fixed-save','id-run'] if name in ('proposal-during-args','proposal-after-args') else ['id-save','id-run'] if status=='completed' else ['id-save']),rows
            assert sum(r.get('event')=='model.proposal_retry' for r in rows)==(0 if name=='proposal-http' else 1),rows
        finally:
            if proc.poll() is None:proc.kill();proc.wait()
            os.close(master)
    c.configure(max_rounds=2)
    # With two model rounds only: saved write + rejected proposal, no retry request.
    master,slave=pty.openpty();proc=subprocess.Popen(c.cmd(['chat'],'proposal-budget',True),stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=c.env);os.close(slave)
    try:
        read_until(master,b'> ');os.write(master,b'proposal-budget\n');read_until(master,b'Type yes to allow once: ');os.write(master,b'y\n');read_until(master,b'> ');os.write(master,b'/q\n');out,_=finish_terminal(proc,master)
        assert json.loads(out.splitlines()[-1])['status']=='budget'
        assert c.server.prompts.count('proposal-budget')==2
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        os.close(master)
    c.configure(enabled_tools=['write_file','shell'],stream=True,max_tokens=256)
    master,slave=pty.openpty();proc=subprocess.Popen(c.cmd(['chat'],'proposal-crash',True),stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=c.env);os.close(slave)
    try:
        read_until(master,b'> ');os.write(master,b'proposal-crash\n');read_until(master,b'Type yes to allow once: ');os.write(master,b'y\n')
        with c.server.received:
            assert c.server.received.wait_for(lambda:c.server.prompts.count('proposal-crash')==3,timeout=5)
        inode=(c.work/'parameterized.sh').stat().st_ino
        proc.kill();proc.wait()
        assert 'already consumed' in c.execute(['resume'],1,session='proposal-crash')['text']
        assert c.server.prompts.count('proposal-crash')==3 and (c.work/'parameterized.sh').stat().st_ino==inode
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        os.close(master)
    # Ready is resumable; only the remaining unexecuted call is proposed, requiring approval.
    state=json.loads((c.root/'sessions/proposal-ok.json').read_text())
    assert state['version']==2
    state.update(active=True,rounds=2,messages=state['messages'][:3],proposal_retry={'phase':'ready'})
    path=c.root/'sessions/proposal-ready.json';path.write_text(json.dumps(state));path.chmod(0o600)
    inode=(c.work/'parameterized.sh').stat().st_ino
    assert c.execute(['resume'],3,session='proposal-ready')['status']=='denied'
    assert (c.work/'parameterized.sh').stat().st_ino==inode
    c.configure()
    print('PASS typed provider format retry, one regeneration, paired write not replayed, corrected approval/denial, repeated rejection, HTTP exclusion, round cap, sent crash seal and ready resume')

def execution_file_recovery(c):
    c.configure(enabled_tools=['shell','write_file'],max_rounds=6)
    for name,status,approvals in [('ok','completed',4),('deny-fix','denied',2),('deny-run','denied',3),('twice','failed',2)]:
        (c.work/'repaired.sh').write_text('exit 17')
        prompt='exec-file-repair-'+name
        master,slave=pty.openpty();proc=subprocess.Popen(c.cmd(['chat'],prompt,True),stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=c.env);os.close(slave)
        try:
            read_until(master,b'> ');os.write(master,prompt.encode()+b'\n')
            for index in range(approvals):
                review=read_until(master,b'Type yes to allow once: ')
                assert b'repaired.sh' in review
                deny=(name=='deny-fix' and index==1) or (name=='deny-run' and index==2)
                os.write(master,b'n\n' if deny else b'y\n')
            read_until(master,b'> ');os.write(master,b'/q\n');out,_=finish_terminal(proc,master)
            rows=[json.loads(line) for line in out.splitlines()]
            assert rows[-1]['status']==status,rows
            expected=4 if name=='ok' else 1 if name=='deny-fix' else 2
            c.server.assert_healthy()
            if name == 'twice':
                assert 'transport' not in rows[-1]['text'].lower(), rows
            assert sum(r.get('event')=='tool.start' for r in rows)==expected,rows
            assert sum(r.get('event')=='tool.execution_failed' for r in rows)==1
        finally:
            if proc.poll() is None:proc.kill();proc.wait()
            os.close(master)
    state=json.loads((c.root/'sessions/exec-file-repair-ok.json').read_text())
    state.update(active=True,rounds=2,messages=state['messages'][:5]);state['repair']['phase']='preparing'
    path=c.root/'sessions/exec-prepared-resume.json';path.write_text(json.dumps(state));path.chmod(0o600)
    inode=(c.work/'repaired.sh').stat().st_ino
    assert c.execute(['resume'],3,session='exec-prepared-resume')['status']=='denied'
    assert (c.work/'repaired.sh').stat().st_ino==inode
    c.configure()
    print('PASS one file repair before original execution retry, remaining task continuation, every approval, repeated preparation rejected and prepared resume without rewrite')


def history_window(c):
    c.configure(enabled_tools=['shell','write_file'],stream=True,max_messages=8,system_prompt='window policy',tool_result_prompt='window guidance')
    for i in range(3):c.run('old-'+str(i),session='window-scope')
    prior=json.loads((c.root/'sessions/window-scope.json').read_text())['messages']
    master,slave=pty.openpty();proc=subprocess.Popen(c.cmd(['chat'],'window-scope',True),stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=c.env);os.close(slave)
    try:
        read_until(master,b'> ');os.write(master,b'proposal-window\n')
        for i in range(2):read_until(master,b'Type yes to allow once: ');os.write(master,b'y\n')
        read_until(master,b'> ');os.write(master,b'/q\n');out,_=finish_terminal(proc,master)
        rows=[json.loads(line) for line in out.splitlines()];assert rows[-1]['status']=='completed',rows
        assert any(r.get('event')=='context.trimmed' for r in rows)
        state=json.loads((c.root/'sessions/window-scope.json').read_text())
        assert state['messages'][:len(prior)]==prior and len(state['messages'])>8
        assert [r['data'] for r in rows if r.get('event')=='tool.start']==['id-save','id-run']
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        os.close(master)
    c.configure(enabled_tools=['shell','write_file'],stream=True,max_messages=4)
    master,slave=pty.openpty();proc=subprocess.Popen(c.cmd(['chat'],'window-small',True),stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=c.env);os.close(slave)
    try:
        read_until(master,b'> ');os.write(master,b'proposal-small\n');read_until(master,b'Type yes to allow once: ');os.write(master,b'y\n')
        read_until(master,b'> ');os.write(master,b'/q\n');out,_=finish_terminal(proc,master)
        rows=[json.loads(line) for line in out.splitlines()];assert rows[-1]['status']=='budget',rows
        assert [r['data'] for r in rows if r.get('event')=='tool.start']==['id-save']
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        os.close(master)
    c.configure()
    print('PASS bounded model history window preserves journal and current tool pairs; current-only overflow stays budget without dispatch')


def image_turn(c):
    path = c.root / 'pixel.png'
    path.write_bytes(base64.b64decode(req_png_url.split(',')[1]))
    c.configure(stream=True, max_tokens=64, enable_thinking=False,temperature=0)
    row = c.execute(['--image',str(path),'run','picture'], session='picture-test')
    assert row['text'] == 'results=51'
    path.unlink()
    assert c.execute(['resume','recall-picture'], session='picture-test')['text'] == 'image retained'
    path.write_bytes(b'invalid image')
    c.execute(['--image',str(path),'run','picture'], 1)
    print('PASS bounded PNG + tool, request token cap, restart without source file, malformed image rejection')


def argument_repair(c):
    limits = dict(max_rounds=3, max_tokens=96, enable_thinking=False, max_context_bytes=8192)
    for stream in (False, True):
        c.configure(stream=stream, **limits)
        cases = [('ok',0,3), ('json',0,3), ('batch',0,3), ('twice',1,2),
                 ('text',1,2), ('switch',1,2), ('reuse-id',1,2), ('approval',3,2)]
        for variant, code, n in cases:
            prompt = 'repair-' + variant
            old = c.server.prompts.count(prompt)
            rows = c.run(prompt, code, events=True)
            assert c.server.prompts.count(prompt) == old+n
            assert [r['data'] for r in rows if r.get('event') == 'tool.invalid_arguments'] == ['id-bad']
            starts = [r['data'] for r in rows if r.get('event') == 'tool.start']
            assert starts == (['id-first','id-new'] if variant == 'batch' else ['id-new'] if not code else [])
            if not code: assert rows[-1]['text'] == '16353'
            if variant == 'twice': assert 'calc.op contains extra quotes' in rows[-1]['text']
            saved = c.execute(['resume'], code, session=rows[-1]['session'])
            assert saved == rows[-1] and c.server.prompts.count(prompt) == old+n
    assert not (c.work / 'created').exists()
    c.configure(stream=True, **limits)
    session = 'repair-reset'
    assert c.run('multiple',session=session)['text']=='results=你好|42'
    for _ in range(2):
        assert c.run('repair-ok',session=session)['text'] == '16353'
    c.configure(max_rounds=1, max_tokens=96, enable_thinking=False)
    assert 'round limit' in c.run('repair-ok',4)['text']
    c.configure(max_output_bytes=32, max_tokens=96, enable_thinking=False)
    assert 'feedback exceeds budget' in c.run('repair-ok',4)['text']
    c.configure(max_messages=3, max_tokens=96, enable_thinking=False)
    old = c.server.prompts.count('repair-ok')
    assert 'correction request exceeds context budget' in c.run('repair-ok',4)['text']
    assert c.server.prompts.count('repair-ok') == old + 1
    c.configure(**limits)
    prompt = 'repair-crash'
    proc = subprocess.Popen(c.cmd(['run',prompt],prompt), env=c.env, stdout=subprocess.PIPE)
    try:
        with c.server.received:
            assert c.server.received.wait_for(lambda: c.server.prompts.count(prompt)==2, timeout=5)
        proc.kill()
        proc.wait(timeout=5)
        assert 'already consumed' in c.execute(['resume'],1,session=prompt)['text']
        assert c.server.prompts.count(prompt) == 2
    finally:
        if proc.poll() is None: proc.kill(); proc.wait()
    print('PASS one paired argument repair, concrete errors, batch ordering, fresh ID, same-tool enforcement, denial, budgets and crash without extra retry')


def local_tasks(c):
    for stream in [True, False]:
        c.configure(stream=stream,system_prompt="Use tools for local tasks.",max_tokens=96)
        args=['--require-tool','echo','run']
        session='required-'+str(stream)
        assert c.execute(args+['require-choice'],session=session)['text']=='results=observed'
        for prompt,name,code in [('omission-ok','echo',0),('omission-denied','write_file',3)]:
            old=c.server.prompts.count(prompt)
            rows=c.execute(['--require-tool',name,'run',prompt], code=code, events=True)
            assert c.server.prompts.count(prompt)==old+(2 if code else 3)
            assert [r['data'] for r in rows if r.get('event')=='tool.required_retry']==[name]
            assert 'fabricated' not in str(rows)
            if code: assert rows[-1]['status']=='denied' and not (c.work/'never').exists()
            else: assert rows[-1]['text']=='results=observed'
            saved=json.loads((c.root/'sessions'/(rows[-1]['session']+'.json')).read_text())
            assert sum(m['role']=='user' for m in saved['messages'])==1 and 'fabricated' not in str(saved)
        for prompt in ['invented-observation','multiple']:
            old=c.server.prompts.count(prompt)
            rows=c.execute(args+[prompt],code=1,session=session,events=True)
            assert 'nothing executed' in rows[-1]['text'], rows
            # Every attempted model request now emits independent usage/byte
            # metadata, including rejected proposals. No text/tool event may
            # escape a twice-rejected required-tool proposal.
            assert [r['event'] for r in rows[:-1] if r['event'] != 'model.metrics']==['turn.start','tool.required_retry','turn.end'], rows
            measurements=[json.loads(r['data']) for r in rows[:-1] if r['event']=='model.metrics']
            assert len(measurements)==2 and all(m['request_bytes']>0 and m['response_bytes']>0 and m['elapsed_ms']>=0 for m in measurements), rows
            assert c.server.prompts.count(prompt)==old+2
        saved=json.loads((c.root/'sessions'/(session+'.json')).read_text())
        assert 'reply:invented-observation' not in [m.get('content') for m in saved['messages']]
        c.execute(['--require-tool','absent','run','none'],code=64)
    c.configure(tool_result_prompt='next-step-context',tool_result_for='echo')
    assert c.run('previous-topic',session='transient-context')['text']=='reply:previous-topic'
    row=c.execute(args+['require-choice'],session='transient-context')
    assert row['text']=='observed'
    state=json.loads((c.root/'sessions'/'transient-context.json').read_text())
    assert 'next-step-context' not in str(state)
    c.configure(tool_result_prompt='next-step-context',tool_result_for='read_file')
    assert c.execute(args+['require-choice'])['text']=='results=observed'
    # Kill the process after the server receives the one permitted correction.
    c.configure(max_tokens=96)
    prompt='omission-crash'
    proc=subprocess.Popen(c.cmd(['--require-tool','echo','run',prompt],prompt),env=c.env,stdout=subprocess.PIPE)
    try:
        with c.server.received:
            assert c.server.received.wait_for(lambda: c.server.prompts.count(prompt)==2, timeout=5)
        proc.kill(); proc.wait(timeout=5)
        assert 'already consumed' in c.execute(['resume'],1,session=prompt)['text']
        assert c.server.prompts.count(prompt)==2
    finally:
        if proc.poll() is None: proc.kill(); proc.wait()
    c.configure(max_tokens=96,max_rounds=1)
    old=c.server.prompts.count('omission-ok')
    assert 'round limit' in c.execute(args+['omission-ok'],4)['text']
    assert c.server.prompts.count('omission-ok')==old+1
    c.configure(max_tokens=96)
    master,slave=pty.openpty()
    proc=subprocess.Popen(c.cmd(['chat'],'scoped-tool',True),stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=c.env)
    os.close(slave)
    try:
        read_until(master,b'> ')
        for line in (b'/tool echo omission-ok',b'hello',b'/tool disabled hello'):
            os.write(master,line+b'\n')
            read_until(master,b'> ')
        os.write(master,b'/q\n')
        out=finish_terminal(proc,master,timeout=5)[0]
        rows=[json.loads(line.decode()) for line in out.splitlines()]
        assert [r['text'] for r in rows if r.get('status')=='completed']==['results=observed','reply:hello']
        assert [r['data'] for r in rows if r.get('event')=='tool.required_retry']==['echo']
        assert rows[-1]['status']=='error' and c.server.prompts.count('disabled hello')==0
    finally:
        if proc.poll() is None: proc.kill(); proc.wait()
        os.close(master)
    print('PASS required tool omission, single same-turn retry, repeated omission, wrong tool, exact approval, crash and round limit; /tool ends at next greeting')
    c.configure(enabled_tools=['shell'])
    assert c.run('shell-error',3)['status']=='denied'
    assert pty_turn(c,'shell-error','shell-nonzero') == 'results=exit_code=201\nobserved-error'
    stopped=json.loads((c.root/'sessions/shell-nonzero.json').read_text())
    assert stopped['status']==1 and not stopped['active']
    assert any('observed-error' in m.get('content','') for m in stopped['messages'] if m['role']=='tool')
    print('PASS named first tool, wire tool_choice, blocked fabricated text/wrong tool with no streamed or stored answer, per-turn scope, shell denial, approved nonzero exit and stderr feedback')


def errors(c):
    for stream in [True, False]:
        c.configure(stream=stream)
        for prompt, code in [('unknown',1), ('invalid_args',1), ('bad_args_json',1), ('duplicate',1),
                             ('malformed',1), ('rate',1), ('unsupported',1), ('redirect',1), ('disconnect',1),
                             ('exec',3), ('write',3)]:
            old = c.server.prompts.count(prompt)
            r = c.run(prompt, code)
            expected = 2 if prompt in ('invalid_args','bad_args_json') else 1
            assert c.server.prompts.count(prompt) == old + expected
            if prompt == 'rate': assert '429' in r['text'], r
            if prompt == 'redirect': assert '307' in r['text'], r
        assert not (c.work / 'created').exists()
    c.configure(stream=True)
    assert 'SSE' in c.run('truncated', 1)['text']
    c.configure(timeout_ms=120)
    c.run('slow', 7)
    c.configure(max_response_bytes=1024)
    c.run('large', 4)
    for limits, prompt in [({'max_rounds':2},'forever'), ({'max_calls':1},'multiple'),
                           ({'max_messages':1},'single'), ({'max_message_bytes':2},'too long'),
                           ({'max_context_bytes':10},'too long'), ({'max_output_bytes':64},'outlimit')]:
        c.configure(**limits)
        c.run(prompt, 4)
    c.configure(max_calls=1)
    assert '2 calls; max_calls is 1' in c.run('multiple',4)['text']
    c.configure(max_calls=4)
    batch=c.run('multiple',events=True)
    tool_events=[(r['event'],r['data']) for r in batch if r.get('event','').startswith('tool.')]
    assert tool_events == [
        ('tool.pre_execute','id-0'),('tool.start','id-0'),('tool.post_execute','id-0'),
        ('tool.finalized','id-0'),('tool.result','id-0'),
        ('tool.pre_execute','id-1'),('tool.start','id-1'),('tool.post_execute','id-1'),
        ('tool.finalized','id-1'),('tool.result','id-1')],tool_events
    assert batch[-1]['text']=='results=你好|42'
    c.configure()
    (c.work / 'safe').write_text('inside')
    (c.work / 'escape').symlink_to(c.cfg)
    assert c.run('file safe')['text'] == 'results=inside'
    for target in ['../config.json','escape','/etc/passwd']: c.run('file ' + target, 1)
    c.configure()
    del c.env['DUNLIN_TEST_BEARER']
    assert 'credential' in c.run('single', 1)['text']
    c.env['DUNLIN_TEST_BEARER'] = c.server.bearer
    print('PASS invalid responses and arguments, unknown tools, rate limit, credential, disconnect, timeout, budgets, paths, noninteractive denial')


def tls_failure(c):
    cert, key = str(c.root / 'test.crt'), str(c.root / 'test.key')
    subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1',
                    '-subj','/CN=localhost','-keyout',key,'-out',cert], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    srv = MockServer()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert,key)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    thread = threading.Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    try:
        c.configure(base_url='https://127.0.0.1:{}/v1'.format(srv.server_port))
        assert 'TLS' in c.run('private',1)['text']
        assert not srv.prompts
    finally:
        srv.shutdown()
        srv.server_close()
        srv.assert_healthy()
        c.configure()
    print('PASS untrusted HTTPS certificate rejected before sending model content')


def signal_model(c, prompt, signo):
    with c.server.received:
        old = c.server.prompts.count(prompt)
    p = subprocess.Popen(c.cmd(['run',prompt], prompt), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         env=c.env)
    try:
        with c.server.received:
            assert c.server.received.wait_for(lambda: c.server.prompts.count(prompt) > old, timeout=5)
        if signo == signal.SIGKILL:
            c.execute(['resume'], 6, session=prompt)
        p.send_signal(signo)
        out, err = p.communicate(timeout=5)
        if signo == signal.SIGINT:
            assert p.returncode == 2 and json.loads(out.decode('utf-8'))['session'] == prompt, (p.returncode,out,err)
        else: assert p.returncode == -signo
    finally:
        if p.poll() is None: p.kill(); p.wait()


def read_until(master, value):
    end = time.monotonic()+8
    data = b''
    while not (re.search(rb'(?:^|[\r\n])> $',data) if value==b'> ' else value in data):
        assert select.select([master],[],[], max(0,end-time.monotonic()))[0], (value, data)
        chunk = os.read(master,8192)
        assert chunk, (value, data, 'terminal closed')
        data += chunk
    return data


def chat_utf8(c):
    c.configure()
    question = '你帮我看一下当前系统 的python版本是多少？        '
    for initial in ('-iutf8', 'iutf8'):
        master, slave = pty.openpty()
        subprocess.check_call(['stty', initial], stdin=slave)
        before = termios.tcgetattr(slave)
        session = 'chat-' + initial
        proc = subprocess.Popen(c.cmd(['chat'],session), stdin=slave, stderr=slave,
                                stdout=subprocess.PIPE, env=c.env)
        try:
            read_until(master, b'> ')
            erase = before[6][termios.VERASE]
            os.write(master, '你好\n'.encode())
            read_until(master, b'> ')
            os.write(master, b'broken\xff\n')
            read_until(master, b'> ')
            encoded = question.encode().replace('统'.encode(), '错'.encode()+erase+'🙂'.encode()+erase+'统'.encode())
            os.write(master, encoded + b'\n')
            read_until(master, b'> ')
            os.write(master, before[6][termios.VEOF])
            out, _ = finish_terminal(proc,master,timeout=5)
            assert proc.returncode == 0 and termios.tcgetattr(slave) == before
            rows = [json.loads(line.decode()) for line in out.splitlines()]
            assert [r['status'] for r in rows] == ['completed','error','completed'], rows
            assert rows[0]['text'] == 'reply:你好' and rows[2]['text'] == 'reply:'+question, rows
            saved = json.loads((c.root/'sessions'/(session+'.json')).read_text())
            assert [m['content'] for m in saved['messages'] if m['role']=='user'] == ['你好',question]
        finally:
            if proc.poll() is None: proc.kill(); proc.wait()
            os.close(slave)
            os.close(master)
    p = subprocess.run(c.cmd(['chat']), input=b'bad\xff\n', stdout=subprocess.PIPE, env=c.env)
    assert p.returncode == 1 and json.loads(p.stdout.decode())['text'] == 'invalid UTF-8 input'
    print('PASS UTF-8 PTY backspace (CJK + emoji), exact mixed input, retry after malformed line, terminal restoration and strict piped chat')


def chat_budget(c):
    c.configure()
    master, slave = pty.openpty()
    p = subprocess.Popen(c.cmd(['chat'],'tty-budget'), stdin=slave, stderr=slave, stdout=subprocess.PIPE, env=c.env)
    os.close(slave)
    try:
        read_until(master,b'> ')
        os.write(master,b'output-length\n')
        read_until(master,b'> ')
        os.write(master,b'after-budget\n')
        read_until(master,b'> ')
        os.write(master,b'\x04')
        out,_=finish_terminal(p,master,timeout=5)
        rows=[json.loads(line.decode()) for line in out.splitlines()]
        assert p.returncode==0 and [r['status'] for r in rows]==['budget','completed']
        assert rows[1]['text']=='reply:after-budget'
    finally:
        if p.poll() is None: p.kill(); p.wait()
        os.close(master)
    batch=subprocess.run(c.cmd(['chat']),input=b'output-length\nafter-budget\n',env=c.env,stdout=subprocess.PIPE)
    assert batch.returncode==4 and json.loads(batch.stdout.decode())['status']=='budget'
    print('PASS token budget returns to TTY prompt and next turn completes; piped chat exits 4')


def chat_exit(c):
    c.configure()
    for i, word in enumerate(('/q', '  /quit  ', '/exit')):
        session='tty-exit-'+str(i)
        master, slave=pty.openpty()
        before=termios.tcgetattr(slave)
        proc=subprocess.Popen(c.cmd(['chat'],session,events=True), stdin=slave, stderr=slave,
                              stdout=subprocess.PIPE, env=c.env)
        try:
            read_until(master,b'> ')
            sent=len(c.server.prompts)
            os.write(master,word.encode()+b'\n')
            out,_=finish_terminal(proc,master,timeout=2)
            assert proc.returncode==0 and out==b'' and len(c.server.prompts)==sent, (word,out)
            assert not (c.root/'sessions'/(session+'.json')).exists()
            assert termios.tcgetattr(slave)==before
        finally:
            if proc.poll() is None: proc.kill(); proc.wait()
            os.close(master); os.close(slave)
    master,slave=pty.openpty()
    proc=subprocess.Popen(c.cmd(['chat'],'tty-exit-prose'),stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=c.env)
    try:
        read_until(master,b'> ')
        os.write(master,b'exit the program?\n')
        read_until(master,b'> ')
        os.write(master,b'/quit\n')
        out,_=finish_terminal(proc,master,timeout=5)
        assert proc.returncode==0 and json.loads(out.decode())['text']=='reply:exit the program?'
    finally:
        if proc.poll() is None: proc.kill(); proc.wait()
        os.close(master); os.close(slave)
    # Batch input is data. Local control parsing belongs only to the interactive UI.
    rows=c.execute(['chat'],session='batch-exit',events=True,stdin=b'exit\n')
    assert rows[-1]['text']=='reply:exit'
    print('PASS all local TTY exit forms: no request/events/checkpoint, terminal restored, piped exit stays data')


def pty_turn(c, prompt, session, crash=False):
    master, slave = pty.openpty()
    proc = subprocess.Popen(c.cmd(['chat'],session), stdin=slave, stderr=slave,
                            stdout=subprocess.PIPE, env=c.env)
    os.close(slave)
    try:
        read_until(master, b'> ')
        os.write(master, prompt.encode() + b'\n')
        approval = read_until(master, b'Type yes to allow once')
        if prompt == 'shell-error':
            assert b'printf observed-error >&2; exit 201' in approval
        if prompt == 'write':
            assert b'created' in approval and b'approved' in approval
        if prompt == 'exec':
            assert b'literal;$(not-run)' in approval
        os.write(master,b'yes\n')
        if crash:
            marker = c.work / 'marker'
            end = time.monotonic()+5
            while not marker.exists() and time.monotonic() < end: time.sleep(0.01)
            assert marker.read_text() == 'launch\n'
            proc.kill()
            proc.wait(timeout=4)
        else:
            read_until(master, b'> ')
            os.write(master,b'\x04')
            out, _ = finish_terminal(proc,master,timeout=5)
            assert proc.returncode == 0, (proc.returncode,out)
            return json.loads(out.decode())['text']
    finally:
        if proc.poll() is None: proc.kill(); proc.wait()
        os.close(master)


def recovery(c):
    c.configure()
    signal_model(c, 'cancel', signal.SIGINT)
    signal_model(c, 'kill_model', signal.SIGKILL)
    c.server.wait_seconds = 0.01
    assert c.execute(['resume'],session='kill_model')['text'] == 'reply:kill_model'
    assert pty_turn(c, 'write', 'writeok') == 'results=written'
    assert (c.work / 'created').read_text() == 'approved'
    assert 'literal;$(not-run)' in pty_turn(c,'exec','execok')
    pty_turn(c, 'crash_tool','unknown-effect', crash=True)
    n = len(c.server.prompts)
    c.execute(['resume'], 5, session='unknown-effect')
    c.execute(['resume'], 5, session='unknown-effect')
    assert c.run('new input', 5, session='unknown-effect')['status'] == 'unknown'
    assert (c.work / 'marker').read_text() == 'launch\n' and n == len(c.server.prompts)
    print('PASS SIGINT, concurrent lease exclusion, SIGKILL before response and safe resume, PTY exact-argument approval, durable unknown after real effect without replay')


if __name__ == '__main__':
    srv = MockServer(bearer='ephemeral-integration-fixture')
    thread = threading.Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='dunlin-e2e-') as tmp:
            case = Case(sys.argv[1], tmp, srv)
            basics(case)
            fresh_sessions(case)
            cli_frontend(case)
            quoted_command_followup(case)
            proposal_retry(case)
            execution_file_recovery(case)
            history_window(case)
            turn_boundary(case)
            command_repair(case)
            command_recovery(case)
            chat_utf8(case)
            chat_budget(case)
            chat_exit(case)
            image_turn(case)
            argument_repair(case)
            local_tasks(case)
            errors(case)
            tls_failure(case)
            recovery(case)
    finally:
        srv.shutdown()
        srv.server_close()
        srv.assert_healthy()
