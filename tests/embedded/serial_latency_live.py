"""Exclusive ordinary ESP32 validation; no credentials/settings rewrite."""
import argparse,json,re,time,termios,datetime,email.utils,http.client
from pathlib import Path
import serial
p=argparse.ArgumentParser();p.add_argument('--port',required=True);p.add_argument('--output',required=True);a=p.parse_args()
s=serial.Serial(port=None,baudrate=115200,timeout=.05,exclusive=True);s.dtr=False;s.rts=False;s.port=a.port
raw=bytearray();cases=[];disconnected=False
with s:
 flags=termios.tcgetattr(s.fileno());flags[2]&=~termios.HUPCL;termios.tcsetattr(s.fileno(),termios.TCSANOW,flags)
 def drain(seconds):
  until=time.monotonic()+seconds
  while time.monotonic()<until:raw.extend(s.read(4096))
 def run(prompt,model=False):
  start=len(raw);s.write((prompt+'\n').encode());s.flush();until=time.monotonic()+180
  pattern=re.escape('[任务结束] status=').encode()+rb'(?:replied|observed|truncated|stopped);[^\r\n]*\r?\n' if model or not prompt.startswith('/') else rb'job=\d+ (?:ok|busy|invalid|unsupported|io_error):[^\r\n]*\r?\n'
  while not re.search(pattern,raw[start:]):
   assert time.monotonic()<until,'timeout '+prompt;raw.extend(s.read(4096))
  drain(.3);text=bytes(raw[start:]).decode('utf8');assert '(null)' not in text and '\x00' not in text and 'Guru Meditation' not in text,text
  cases.append({'prompt':prompt,'text':text});print('COMMAND '+prompt+'\n'+text,flush=True);return text
 try:
  drain(5);run('/model clear')
  begin=len(raw);start=time.monotonic();s.write('逐行输出1到180，每行写“串口压力测试”，不要省略，不调用工具。\n'.encode());s.flush()
  while b'| 1' not in raw[begin:]:
   assert time.monotonic()-start<120;raw.extend(s.read(4096))
  sent=time.monotonic();s.write(b'/status\n');s.flush()
  while not re.search(rb'job=\d+ ok: heap_free=[^\r\n]+\r?\n',raw[begin:]):
   assert time.monotonic()-sent<90;raw.extend(s.read(4096))
  latency=time.monotonic()-sent;drain(2)
  while not re.search(re.escape('[任务结束] status='.encode())+rb'[^\r\n]+\r?\n',raw[begin:]):
   assert time.monotonic()-start<180;raw.extend(s.read(4096))
  drain(.2)
  result={'prompt':'/status during active long-text transmission','latency_s':latency,'text':bytes(raw[begin:]).decode('utf8')};cases.append(result);print(json.dumps(result,ensure_ascii=False),flush=True)
  assert 'conversation cleared' in run('/model clear');print('PASS status eventually returns after active output; latency is measured, not asserted against an invented budget',flush=True)
 finally:
  if disconnected:
   s.write(b"/wifi connect\n");s.flush();drain(5)
  Path(a.output+'.bin').write_bytes(raw);Path(a.output+'.json').write_text(json.dumps(cases,ensure_ascii=False,indent=2)+'\n')
