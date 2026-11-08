"""PTY evidence of live child output, terminal capabilities and quiet refresh."""
import os,pty,select,subprocess,sys,tempfile,threading,time,re,fcntl,termios,struct
from model_e2e import Case,finish_terminal
from mock_model import MockServer
from intent_progress import read_to

def run(case,term,no_color):
    master,slave=pty.openpty()
    fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',24,100,0,0))
    env=dict(case.env,TERM=term)
    if no_color: env['NO_COLOR']='1'
    else: env.pop('NO_COLOR',None)
    marker=case.work/'ui-finished'
    if marker.exists(): marker.unlink()
    proc=subprocess.Popen([case.exe,'--config',str(case.cfg),'chat'],stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=env)
    os.close(slave)
    try:
        read_to(master,b'> ');os.write(master,b'ui-stages\n')
        proposal=read_to(master,b'Type yes to allow once: ')
        assert b'sleep 1' in proposal
        os.write(master,b'yes\n')
        data=bytearray(); seen=set();deadline=time.monotonic()+15
        while b'completed' not in data or not bytes(data).endswith(b'> '):
            assert time.monotonic()<deadline,bytes(data)
            if select.select([master],[],[],.05)[0]: data.extend(os.read(master,65536))
            plain=re.sub(rb'\x1b\[[0-9;]*m',b'',bytes(data))
            if term!='dumb':
                for phase in (b'phase-one',b'phase-two'):
                    if phase not in seen and any(phase in frame and frame.startswith('    执行'.encode()) for frame in plain.split(b'\r\x1b[2K')):
                        assert not marker.exists() and proc.poll() is None,bytes(data)
                        seen.add(phase)
        assert marker.read_text()=='done'
        if term!='dumb': assert len(seen)==2,(seen,bytes(data))
        assert '[执行中]'.encode() not in data and b'[Reasoning]' not in data
        assert bool(re.search(rb'\x1b\[[0-9;]*m',data))==(term!='dumb' and not no_color),bytes(data)
        if term=='dumb': assert b'\x1b[2K' not in data
        assert bytes(data).count(b'\n')<15,bytes(data)
        os.write(master,b'/q\n');output,_=finish_terminal(proc,master,timeout=8)
        assert b'finished' in output and proc.returncode==0
        print('PASS live stdout/stderr before completion, quiet refresh, hierarchy',term,'NO_COLOR='+str(no_color))
    finally:
        if proc.poll() is None: proc.kill();proc.wait()
        os.close(master)

def repeat_guard(case):
    master,slave=pty.openpty()
    capture=tempfile.TemporaryFile()
    proc=subprocess.Popen([case.exe,'--config',str(case.cfg),'--events','chat'],stdin=slave,stderr=slave,stdout=capture,env=case.env)
    os.close(slave)
    try:
        read_to(master,b'> ');os.write(master,b'ui-probe-cycle\n')
        first=read_to(master,b'Type yes to allow once: ')
        assert b'command -v sh' in first,first
        os.write(master,b'yes\n')
        try: second=read_to(master,b'Type yes to allow once: ')
        except AssertionError:
            capture.seek(0);print(capture.read().decode(),flush=True);raise
        assert b'printf progressed' in second and b'command -v sh' not in second,second
        os.write(master,b'yes\n');read_to(master,b'> ');os.write(master,b'/q\n')
        finish_terminal(proc,master,timeout=8);capture.seek(0);output=capture.read();capture.close()
        import json
        rows=[json.loads(line) for line in output.splitlines()]
        assert sum(row.get('event')=='tool.start' for row in rows)==2,rows
        assert sum(row.get('event')=='tool.invalid_arguments' for row in rows)==1,rows
        assert rows[-1]['status']=='completed' and rows[-1]['text']=='progressed',rows
        print('PASS duplicate successful probe corrected before approval/execution; actual continuation and machine JSON')
    finally:
        if proc.poll() is None: proc.kill();proc.wait()
        os.close(master)

if __name__=='__main__':
    server=MockServer(bearer='isolated-ui-fixture');worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
    try:
        with tempfile.TemporaryDirectory() as directory:
            case=Case(sys.argv[1],directory,server);case.configure(enabled_tools=['shell'],tool_timeout_ms=10000)
            repeat_guard(case)
            for term,color in [('xterm-256color',False),('xterm-256color',True),('dumb',False)]: run(case,term,color)
        server.assert_healthy()
    finally: server.shutdown();worker.join();server.server_close()
