"""Interactive CLI boundary regression; isolated loopback model, no tools/effects."""
import json,os,pty,subprocess,sys,tempfile,threading
from model_e2e import Case,read_until,finish_terminal
from mock_model import MockServer

def run(exe):
 server=MockServer(bearer='isolated-slash-fixture')
 threading.Thread(target=server.serve_forever,daemon=True).start()
 try:
  with tempfile.TemporaryDirectory(prefix='dunlin-slash-') as root:
   c=Case(exe,root,server);c.configure(enabled_tools=['echo'])
   master,slave=pty.openpty();proc=subprocess.Popen(c.cmd(['chat'],'slash-boundary'),stdin=slave,stderr=slave,stdout=subprocess.PIPE,env=c.env);os.close(slave)
   try:
    read_until(master,b'> ')
    for command in ('/help',' /help','/pwd','/permissions','/unknown-fixture',' /unknown-fixture'):
     before=len(server.prompts);os.write(master,command.encode()+b'\n');out=read_until(master,b'> ')
     assert len(server.prompts)==before,(command,server.prompts)
    natural=('help','help model','wifi forget','model forget','led off','ask /help','  help  ','quit','exit')
    for text in natural:
     before=len(server.prompts);os.write(master,text.encode()+b'\n');read_until(master,b'> ')
     assert server.prompts[before:]==[text],(text,server.prompts[before:])
    os.write(master,b'/q\n');stdout,_=finish_terminal(proc,master,timeout=8)
    assert proc.returncode==0
    answers=[json.loads(v) for v in stdout.splitlines() if v.startswith(b'{')]
    assert len(answers)==len(natural)+2,(answers,stdout)
    assert stdout.count(b'unknown local command')==2,stdout
    print('PASS PTY slash-only local dispatch, unknown slash no model request, bare command words exact natural text and sequential conversation')
   finally:
    if proc.poll() is None:proc.kill();proc.wait()
    os.close(master)
 finally:server.shutdown();server.server_close();server.assert_healthy()
if __name__=='__main__':run(sys.argv[1])
