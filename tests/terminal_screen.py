#!/usr/bin/env python3
"""Development-only VT screen replay. Requires pyte==0.8.2, wcwidth==0.2.13."""
import argparse, codecs, fcntl, json, os, pathlib, pty, select, signal, struct
import subprocess, tempfile, termios, time
import pyte

p=argparse.ArgumentParser();p.add_argument('binary');p.add_argument('report');p.add_argument('--docker');p.add_argument('--expect-old-failure',action='store_true');a=p.parse_args()
report=pathlib.Path(a.report);report.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='dunlin-screen-') as d:
    root=pathlib.Path(d);(root/'work').mkdir();(root/'sessions').mkdir(mode=0o700)
    cfg={'base_url':'http://127.0.0.1:9/v1','model':'offline','workspace':str(root/'work'),'session_dir':str(root/'sessions')}
    if a.docker:cfg.update(workspace='/screen/work',session_dir='/screen/sessions')
    (root/'config.json').write_text(json.dumps(cfg));os.chmod(root/'config.json',0o600)
    cmd=[a.binary,'--config',str(root/'config.json'),'chat']
    if a.docker:cmd=['docker','run','--rm','-it','--init','--platform','linux/arm64','--cpus=1','--memory=256m','--pids-limit=64','--read-only','--cap-drop=ALL','--security-opt','no-new-privileges','--user',str(os.getuid())+':'+str(os.getgid()),'-e','TERM=xterm-256color','--mount','type=bind,source='+d+',target=/screen',a.docker,'--config','/screen/config.json','chat']
    master,slave=pty.openpty();fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',18,20,0,0));before=termios.tcgetattr(slave)
    env=dict(os.environ,TERM='xterm-256color',LC_ALL='en_US.UTF-8')
    proc=subprocess.Popen(cmd,stdin=slave,stdout=slave,stderr=slave,env=env,start_new_session=True)
    screen=pyte.Screen(20,18);stream=pyte.Stream(screen);decoder=codecs.getincrementaldecoder('utf-8')('replace');raw=bytearray();shots=[]
    def drain(delay=.15):
        end=time.monotonic()+delay
        while time.monotonic()<end:
            if select.select([master],[],[],min(.03,max(0,end-time.monotonic())))[0]:
                try:data=os.read(master,65536)
                except OSError:break
                if not data:break
                raw.extend(data);stream.feed(decoder.decode(data))
    def send(data):os.write(master,data);drain()
    def shot(name,expected):
        visible=[line.rstrip() for line in screen.display]
        shots.append(dict(name=name,expected=expected,cursor=[screen.cursor.x,screen.cursor.y],screen=visible))
        row=screen.cursor.y
        assert visible[row]==('> '+expected).rstrip(),(name,visible,screen.cursor.x,row)
        assert screen.cursor.x==2+sum(__import__('wcwidth').wcwidth(c) for c in expected),(name,screen.cursor.x)
    try:
        for _ in range(60):
            drain(.1)
            if any(line.startswith('> ') for line in screen.display):break
        else:raise AssertionError('prompt not shown')
        send('中文🙂错abc'.encode());send(b'\x7f'*4);shot('CJK emoji backspace','中文🙂')
        send(b'\x15');shot('Ctrl-U','')
        startrow=screen.cursor.y
        send(('开始'+'中'*15+'🙂').encode());send(b'\x7f'*12);shot('wrapped deletion','开始'+'中'*4)
        assert screen.cursor.y==startrow and all(not s.strip() for s in screen.display[startrow+1:]),screen.display
        send(b'\x15');send(b'abcd');send(b'\x1b[D'*2);send(b'\x1b[3~');send(b'X');send(b'\x05');shot('arrows forward delete Ctrl-E','abXd')
        send(b'\x01Z\x05');shot('Ctrl-A insert Ctrl-E','ZabXd')
        send(b'\x15');send(b'/q\n')
        for _ in range(60):
            drain(.1)
            if proc.poll() is not None:break
        assert proc.wait(timeout=2)==0
        assert termios.tcgetattr(slave)==before,'terminal attributes not restored'
        offset=len(raw)
        proc=subprocess.Popen(cmd,stdin=slave,stdout=slave,stderr=slave,env=env,start_new_session=True)
        for _ in range(60):
            drain(.1)
            if b'> ' in raw[offset:]:break
        else:raise AssertionError('second prompt not shown')
        proc.send_signal(signal.SIGINT)
        for _ in range(60):
            drain(.1)
            if proc.poll() is not None:break
        assert proc.wait(timeout=2)==2,'idle Ctrl-C cancellation status'
        assert termios.tcgetattr(slave)==before,'Ctrl-C terminal attributes not restored'
        shots.append({'name':'idle SIGINT','exit_code':2,'terminal_restored':True})
        result='pass'
    except Exception as error:
        shots.append({'failure':repr(error)});result='fail'
        if proc.poll() is None:proc.terminate()
    finally:
        if proc.poll() is None:
            try:proc.wait(timeout=3)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
        os.close(slave);os.close(master)
        (report/'screen.tty').write_bytes(raw)
        (report/'screen.json').write_text(json.dumps({'status':result,'command':cmd,'pyte':'0.8.2','wcwidth':'0.2.13','snapshots':shots},ensure_ascii=False,indent=2))
    print(result,report/'screen.json')
    if a.expect_old_failure:assert result=='fail','old reproduction unexpectedly passed'
    else:assert result=='pass',shots[-1]
