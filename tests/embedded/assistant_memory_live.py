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
  drain(2);run('/model clear')
  text=run('请自由起一个四个汉字的旅行标题，只输出标题，不要引号或解释。')
  lines=[x[2:].strip() for x in text.splitlines() if x.startswith('| ') and not x.startswith('| [')]
  title=''.join(lines);assert title and len(title)<30,title
  run('记住我喜欢清淡饮食。只简短确认。')
  text=run('请逐字重复你刚才起的旅行标题，只输出标题。');assert title in text,(title,text)
  text=run('请逐行列出从1到300的编号，每行都写：这是旅行清单的一项。不要省略，不要总结，尽量输出全部。')
  assert 'status=truncated' in text and '回复未完' in text,text
  run('/model clear');assert 'conversation=0/6' in run('/model memory')
  print('PASS real assistant-generated title recall across intervening turn; real length finish explicitly truncated; test conversation cleared',flush=True)
 finally:
  if disconnected:
   s.write(b"/wifi connect\n");s.flush();drain(5)
  Path(a.output+'.bin').write_bytes(raw);Path(a.output+'.json').write_text(json.dumps(cases,ensure_ascii=False,indent=2)+'\n')
