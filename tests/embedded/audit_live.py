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
  text=run('今天日期是什么？调用current_time查询，再计算2+3，简短回答两项。')
  assert 'HTTP Date' in text and '[模型] 正在请求' in text and re.search(r'2\s*\+\s*3\s*[=＝]\s*5|(?:结果|答案)[^\n]{0,8}5|\| 5[。\r\n]',text) and 'status=observed' in text,text
  run('/model clear')
  text=run('请逐行列出1到80，每行格式为“编号：青山绿水”，不要省略，不要总结，不要使用工具。')
  answer='\n'.join(x[2:] for x in text.splitlines() if x.startswith('| ') and not x.startswith('| ['))
  nums=re.findall(r'(\d+)\s*[：:、.]',answer);assert nums and len(answer.encode())>1023 and '[display truncated]' not in answer,text
  last=nums[-1]
  text=run('你上一条回复最后一项的编号是多少？只回答那个编号，不要重新生成列表。')
  assert re.search(r'\|\s*'+last+r'\b',text),(last,text)
  text=run('只回复下面文本，不要解释：你好界\b！');assert '| 你好！' in text,text
  text=run('只回复下面文本，不要解释：A🙂\x7fB');assert '| AB' in text,text
  start=len(raw);s.write(b'a'*420+b'\n');s.flush();drain(2);assert b'line discarded' in raw[start:],bytes(raw[start:])
  assert 'NVS+RAM' in run('/status')
  for i in range(8):
   text=run('只回答OK，不要解释。');assert 'status=replied' in text and 'OK' in text,text
  assert 'conversation=6/6' in run('/model memory')
  run('/model clear');assert 'conversation=0/6' in run('/model memory')
  print('PASS compound current_time+calculation, >1023-byte answer tail recall, raw Chinese/emoji backspace, overflow recovery, eight sequential model calls and bounded history',flush=True)
 finally:
  if disconnected:
   s.write(b"/wifi connect\n");s.flush();drain(5)
  Path(a.output+'.bin').write_bytes(raw);Path(a.output+'.json').write_text(json.dumps(cases,ensure_ascii=False,indent=2)+'\n')
