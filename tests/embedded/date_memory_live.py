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
  drain(4)
  assert 'NVS+RAM' in run('/status')
  run('/model clear')
  for prompt in ['/date','今天日期是什么，是星期几？','能查到日期吗']:
   before=datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8)))
   text=run(prompt,True);after=datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8)))
   dates={v.strftime('%Y-%m-%d') for v in [before,after]};weekdays={['星期一','星期二','星期三','星期四','星期五','星期六','星期日'][v.weekday()] for v in [before,after]}
   assert 'status=observed' in text and 'HTTP Date' in text and 'Asia/Shanghai' in text and any(d in text for d in dates) and any(d in text for d in weekdays),text
   assert '[模型] 正在请求' not in text
  run('/model clear')
  prompts=['记住：我的旅行目的地是杭州西湖，玩七天。只需简短确认。','补充：我从昆明出发，预算优先。只需简短确认。','给这次旅行取一个简短标题，并带上代号紫鹭42。','请只回答：我从哪里出发，去哪里，玩几天，预算偏好是什么，刚才标题的代号是什么？']
  for prompt in prompts:text=run(prompt)
  for token in ['昆明','杭州','紫鹭42']:assert token in text,text
  assert '七' in text or '7' in text,text
  assert any(t in text for t in ['预算','省钱','性价比']),text
  assert 'conversation=4/6' in run('/model memory')
  run('/model clear');assert 'conversation=0/6' in run('/model memory')
  text=run('我之前告诉你的旅行代号是什么？如果没有上下文就明确说不知道，不要猜。');assert '紫鹭42' not in text,text
  # Live offline clock must fail closed, then restore existing saved Wi-Fi without changing its credentials.
  run('/wifi disconnect');disconnected=True;text=run('/date',True);assert 'status=stopped' in text and '不会使用模型猜测' in text,text
  run('/wifi connect');disconnected=False;drain(5);text=run('/date',True);assert 'status=observed' in text,text
  assert 'NVS+RAM' in run('/status')
  print('PASS live date/weekday/source, no model date request, four-turn user+assistant recall, clear, offline denial/reconnect, NVS retained, raw UTF8',flush=True)
 finally:
  if disconnected:
   s.write(b"/wifi connect\n");s.flush();drain(5)
  Path(a.output+'.bin').write_bytes(raw);Path(a.output+'.json').write_text(json.dumps(cases,ensure_ascii=False,indent=2)+'\n')
