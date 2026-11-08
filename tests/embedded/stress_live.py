"""Bounded real UART stress. No credential rewrite, arbitrary IO or model restart.
Outputs progressive JSONL and raw bytes; own port exclusively. Counts are workload,
not acceptance budgets or claims of unlimited serial throughput.
"""
import argparse,json,re,time,termios,statistics
from pathlib import Path
import serial
p=argparse.ArgumentParser();p.add_argument('--port',required=True);p.add_argument('--output',required=True);p.add_argument('--models',type=int,default=24);a=p.parse_args()
base=Path(a.output);raw=bytearray();events=[]
s=serial.Serial(port=None,baudrate=115200,timeout=.02,exclusive=True);s.dtr=False;s.rts=False;s.port=a.port
started=time.monotonic();armed=False
def log(event):
 event['at_s']=round(time.monotonic()-started,3);events.append(event)
 with Path(str(base)+'.jsonl').open('a') as f:f.write(json.dumps(event,ensure_ascii=False)+'\n')
 print(json.dumps(event,ensure_ascii=False),flush=True)
def read():
 data=s.read(4096)
 if data:
  raw.extend(data)
  with Path(str(base)+'.bin').open('ab') as f:f.write(data)
  assert not armed or b'ets Jul 29' not in data,'unexpected reboot'
  assert b'Guru Meditation' not in data and b'panic' not in data.lower(),'device panic'
 return data
def drain(seconds):
 end=time.monotonic()+seconds
 while time.monotonic()<end:read()
def decode(start):return bytes(raw[start:]).decode('utf8',errors='strict')
def status_values(text):
 m=re.search(r'heap_free=(\d+) heap_min=(\d+) largest_internal=(\d+) owner_stack_free=(\d+)',text)
 return dict(zip(['heap_free','heap_min','largest_internal','owner_stack_free'],map(int,m.groups()))) if m else {}
def command(prompt,model=False):
 begin=len(raw);t=time.monotonic();s.write((prompt+'\n').encode());s.flush();end=t+180
 pattern=rb'\[\xe4\xbb\xbb\xe5\x8a\xa1\xe7\xbb\x93\xe6\x9d\x9d\] status=[^\r\n]+\r?\n' if model else rb'job=\d+ (?:ok|busy|invalid|unsupported|limit|io_error):[^\r\n]*\r?\n'
 # UTF8 literal avoids regex byte-escape ambiguities.
 if model:pattern=re.escape('[任务结束] status='.encode())+rb'[^\r\n]+\r?\n'
 while not re.search(pattern,raw[begin:]):
  assert time.monotonic()<end,'command timeout '+prompt
  read()
 elapsed=time.monotonic()-t;drain(.08);text=decode(begin)
 assert '(null)' not in text and '\x00' not in text and 'Guru Meditation' not in text,text
 log({'kind':'model' if model else 'local','prompt':prompt,'elapsed_s':round(elapsed,3),'bytes':len(raw)-begin,'status':re.findall(r'\[任务结束\] status=(\w+)',text),'metrics':status_values(text),'text':text})
 return text
def burst(count):
 begin=len(raw);t=time.monotonic();s.write(b'/status\n'*count);s.flush();last=time.monotonic()
 while time.monotonic()-t<100:
  if read():last=time.monotonic()
  if time.monotonic()-last>2:break
 text=decode(begin);accepted=re.findall(r'ok: queued job=(\d+)',text);complete=re.findall(r'job=(\d+) (?:ok|busy|invalid|limit|unsupported|io_error):',text);rejected=len(re.findall(r'busy: not queued',text))
 log({'kind':'burst','sent':count,'elapsed_s':round(time.monotonic()-t,3),'accepted':len(accepted),'completed':len(complete),'not_queued':rejected,'unaccounted':count-len(accepted)-rejected,'fault_notices':text.count('UART RX overflow/frame error'),'model_started': 'model task queued' in text,'metrics':status_values(text),'text':text})
 assert len(accepted)==len(complete),text
 if count<=96:assert count==len(accepted)+rejected,text
 else:assert 'UART RX overflow/frame error' in text and 'model task queued' not in text,text
 s.write(b'\n');s.flush();drain(.2);command('/status')
with s:
 flags=termios.tcgetattr(s.fileno());flags[2]&=~termios.HUPCL;termios.tcsetattr(s.fileno(),termios.TCSANOW,flags)
 try:
  drain(5);armed=True;command('/status');command('/model clear');command('只回答OK，不要解释。',True);command('/status')
  for i in range(60):command('/status')
  for n in [4,16,96,1024]:burst(n)
  # One long model request plus status and a second natural request while busy.
  begin=len(raw);t=time.monotonic();s.write('逐行输出1到180，每行写“串口压力测试”，不要省略，不调用工具。\n'.encode());s.flush()
  while b'model task queued' not in raw[begin:]:
   assert time.monotonic()-t<15;read()
  injected=time.monotonic();s.write('/status\n只回答BUSY_CHECK\n'.encode());s.flush();status_at=None
  while '[任务结束] status='.encode() not in raw[begin:]:
   assert time.monotonic()-t<180;read()
   if status_at is None and b'heap_min=' in raw[begin:]:status_at=time.monotonic()-injected
  drain(2);text=decode(begin)
  log({'kind':'busy_long','elapsed_s':round(time.monotonic()-t,3),'status_latency_s':status_at,'busy_rejected':'busy: model task active' in text,'bytes':len(raw)-begin,'text':text})
  assert 'busy: model task active' in text and status_at is not None,text
  command('/status')
  for i in range(a.models):
   text=command('只回答OK，不要解释。',True);assert 'status=replied' in text and '| OK' in text,text
   if i%6==5:command('/status')
  for payload in [b'a'*800+b'\n',b'/status\x00\n',b'/status\x1b[A\n',b'x'*100+b'\x03\n']:
   begin=len(raw);s.write(payload);s.flush();drain(2);log({'kind':'malformed','bytes_sent':len(payload),'text':decode(begin)});command('/status')
  command('/model memory');command('/model clear');command('/status')
  for i in range(15):drain(10);command('/status')
  command('/date',True);command('/model clear')
  log({'kind':'done','result':'completed workload; inspect burst loss/latency and heap trends separately'})
 finally:
  Path(str(base)+'.json').write_text(json.dumps(events,ensure_ascii=False,indent=2)+'\n')
