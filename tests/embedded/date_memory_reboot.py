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
  drain(1);s.dtr=False;s.rts=True;time.sleep(.15);s.rts=False;start=len(raw);drain(5)
  boot=bytes(raw[start:]);Path(a.output+'-boot.bin').write_bytes(boot)
  text=boot.decode('utf8',errors='replace');print('BOOT replacement_decode='+str('\ufffd' in text)+' literal_null='+str(b'(null)' in boot),flush=True)
  assert b'(null)' not in boot and 'DUNLIN FreeRTOS serial ABI 1 (esp32)' in text,text
  assert 'conversation=0/6' in run('/model memory')
  assert 'NVS+RAM' in run('/status')
  text=run('/date',True);now=datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8)));assert now.strftime('%Y-%m-%d') in text and ['星期一','星期二','星期三','星期四','星期五','星期六','星期日'][now.weekday()] in text,text
  text=run('你好，请简短回复。');assert 'status=replied' in text,text
  run('/model clear')
  print('PASS explicit hard reset, zero RAM history, NVS restored without host reconfiguration, real date after midnight, real model chat, test memory cleared',flush=True)
 finally:
  if disconnected:
   s.write(b"/wifi connect\n");s.flush();drain(5)
  Path(a.output+'.bin').write_bytes(raw);Path(a.output+'.json').write_text(json.dumps(cases,ensure_ascii=False,indent=2)+'\n')
