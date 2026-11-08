"""Real CLI/child process heartbeat, with only the model HTTP boundary replaced."""
import json
import os
import pty
import select
import subprocess
import sys
import tempfile
import threading
import time
from model_e2e import Case, finish_terminal
from mock_model import MockServer


def read_to(fd, marker, timeout=20):
    data=bytearray()
    deadline=time.monotonic()+timeout
    while marker not in data:
        assert time.monotonic()<deadline, bytes(data)
        if select.select([fd],[],[],0.1)[0]:
            data.extend(os.read(fd,65536))
    return bytes(data)


def exercise(case, events):
    master,slave=pty.openpty()
    command=[case.exe,'--config',str(case.cfg),'--permissions','strict']
    if events: command+=['--events']
    command+=['--session','heartbeat-events' if events else 'heartbeat-natural','chat']
    process=subprocess.Popen(command,stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=case.env)
    os.close(slave)
    try:
        read_to(master,b'> ')
        os.write(master,b'progress-long\n')
        proposal=read_to(master,b'Type yes to allow once: ')
        assert b'sleep 11; printf observed' in proposal
        os.write(master,b'yes\n')
        terminal=read_to(master,b'> ')
        os.write(master,b'/q\n')
        output,_=finish_terminal(process,master,timeout=8)
        assert process.returncode==0,(output,terminal)
        if events:
            rows=[json.loads(line) for line in output.splitlines()]
            ticks=[r['data'] for r in rows if r.get('event')=='exec.progress']
            assert len(ticks)==2,ticks
            assert 'output_bytes=0' in ticks[0],ticks
            assert rows[-1]['status']=='completed' and rows[-1]['text']=='observed',rows
        else:
            assert '[执行中]'.encode() not in terminal and b'completed' in terminal and b'observed' in terminal,terminal
            assert b'observed' in output,output
        case.server.assert_healthy()
    finally:
        if process.poll() is None: process.kill();process.wait()
        os.close(master)


def installation_guard(case):
    for target,version,probe,retry in [('node','v20.fixture','printf probe','model.proposal_retry'),
                                       ('wget','v1.fixture','printf present','tool.required_retry')]:
        case.configure(enabled_tools=['shell'])
        executable=case.work/target
        executable.write_text("#!/bin/sh\nprintf '%s\\n' '"+version+"'\n")
        executable.chmod(0o700)
        session='intent-'+target
        assert case.run('帮我安装wget（回归前文）',session=session)['text']=='ready'
        master,slave=pty.openpty()
        process=subprocess.Popen(case.cmd(['chat'],session=session,events=True),
                                 stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=case.env)
        os.close(slave)
        try:
            read_to(master,b'> ')
            original='那'+target+'呢？'
            os.write(master,(original+'\n').encode())
            first=read_to(master,b'Type yes to allow once: ')
            assert probe.encode() in first,first
            os.write(master,b'yes\n')
            second=read_to(master,b'Type yes to allow once: ')
            assert ('./'+target+' --version').encode() in second and probe.encode() not in second,second
            os.write(master,b'yes\n')
            read_to(master,b'> ')
            os.write(master,b'/q\n')
            output,_=finish_terminal(process,master,timeout=8)
            rows=[json.loads(line) for line in output.splitlines()]
            assert rows[-1]['status']=='completed' and rows[-1]['text']=='verified',rows
            assert sum(row.get('event')=='tool.start' for row in rows)==2,rows
            assert sum(row.get('event')==retry for row in rows)==1,rows
            state=json.loads((case.root/('sessions/'+session+'.json')).read_text())
            assert [m['content'] for m in state['messages'] if m['role']=='user']==['帮我安装wget（回归前文）',original]
        finally:
            if process.poll() is None:
                process.kill();process.wait()
            os.close(master)
    assert case.run('帮我安装wget（回归前文）',session='intent-budget')['text']=='ready'
    case.configure(enabled_tools=['shell'],max_message_bytes=128)
    rows=case.run('那node呢？',4,session='intent-budget',events=True)
    assert rows[-1]['status']=='budget' and not any(r.get('event')=='tool.start' for r in rows),rows
    print('PASS installation intent, repeat rejection before execution, original user text and projection budgets')


if __name__=='__main__':
    server=MockServer(bearer='isolated-heartbeat-fixture')
    worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
    try:
        with tempfile.TemporaryDirectory() as directory:
            case=Case(sys.argv[1],directory,server)
            case.configure(enabled_tools=['shell'],tool_timeout_ms=15000,task_timeout_ms=30000)
            exercise(case,True);exercise(case,False)
            rows=case.run('progress-model',events=True)
            assert any(row.get('event')=='model.progress' for row in rows),rows
            assert rows[-1]['status']=='completed' and rows[-1]['text']=='waited',rows
            installation_guard(case)
        print('PASS silent child emits two five-second heartbeats in events; natural CLI renders command completion, model wait heartbeat; exact final output preserved')
    finally:
        server.shutdown();worker.join();server.server_close();server.assert_healthy()
