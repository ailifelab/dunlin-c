"""Real-model PTY smoke; config/session must be isolated, never approves tools."""
import json,os,pty,subprocess,sys
from model_e2e import read_until,finish_terminal
master,slave=pty.openpty();p=subprocess.Popen([sys.argv[1],'--config',sys.argv[2],'--json','chat'],stdin=slave,stderr=slave,stdout=subprocess.PIPE);os.close(slave)
try:
 read_until(master,b'> ')
 for text in ('/help','/unknown-fixture','请用中文回复：准备好了。不要调用工具。','我们上一轮说了什么？请简短回答，不要调用工具。'):
  os.write(master,text.encode()+b'\n');out=read_until(master,b'> ');print(out.decode('utf8','replace'),flush=True)
 os.write(master,b'/q\n');out,_=finish_terminal(p,master);print(out.decode('utf8','replace'),flush=True)
 rows=[json.loads(v) for v in out.splitlines() if v.startswith(b'{')]
 assert p.returncode==0 and len(rows)==3,rows
 assert all(r['status']=='completed' for r in rows[-2:]),rows
 assert '准备' in rows[-1]['text'],rows
 print('PASS real Qwen no-ask two-turn history; local help and unknown slash')
finally:
 if p.poll() is None:p.kill();p.wait()
 os.close(master)
