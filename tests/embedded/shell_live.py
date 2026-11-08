"""Authorized S3 operator-console regression; no credentials in output."""
import json,re,time,termios,sys
from pathlib import Path
import serial
cfg=json.loads(Path('dunlin-trial/esp32-live.json').read_text())
s=serial.Serial(port=None,baudrate=115200,timeout=.05,exclusive=True);s.dtr=True;s.rts=True;s.port='/dev/cu.usbmodem2101';s.open()
t=termios.tcgetattr(s.fileno());t[2]&=~termios.HUPCL;termios.tcsetattr(s.fileno(),termios.TCSANOW,t)
rx=bytearray();results=[]
def safe(text):
 for key in ('password','key'):
  if cfg.get(key):text=text.replace(cfg[key],'[REDACTED]')
 return text

def wait(pattern,start,timeout=10):
 end=time.monotonic()+timeout
 while True:
  match=re.search(pattern,bytes(rx[start:]))
  if match:return match
  if time.monotonic()>end:raise TimeoutError(safe(bytes(rx[start:]).decode('utf8','replace')))
  rx.extend(s.read(4096))
def begin(line,private=False):
 start=len(rx);s.write((line+'\n').encode());s.flush()
 queued=wait(rb'queued job=(\d+)',start);return start,queued.group(1),time.monotonic()
def local(line,private=False,expected='ok'):
 start,job,began=begin(line,private)
 match=wait(rb'(?:^|[\r\n])job='+job+rb' (ok|invalid|denied|busy|io_error|unsupported|limit):[^\r\n]*\r?\n',start)
 output=bytes(rx[start:]).decode('utf8','replace')
 assert match.group(1).decode()==expected,safe(output)
 label='[private operator configuration]' if private else line
 results.append({'command':label,'status':expected,'completion_without_further_send':True,'observed_after_queue_ms':round((time.monotonic()-began)*1000,2)})
 print('\nCOMMAND '+label+'\n'+safe(output),flush=True);return output
restore='/model set '+cfg['url']+' '+cfg['model']+' '+(cfg.get('key') or '-')
try:
 for c in ('/help','/h','/?','/help wifi','/help model','/help led','/status','/model','/led','/wifi'):
  out=local(c);assert '[模型]' not in out
 local('/model set http://192.0.2.1:1239/v1/chat/completions offline-fixture -')
 for c in ('/help','/h','/help model','/help wifi'):
  out=local(c);assert '[模型]' not in out
 assert 'offline-fixture' in local('/model status')
 assert 'use /help' in local('/unknown-fixture',expected='invalid')
 local('/model forget');assert 'configured=no' in local('/model')
 for text in ('help','wifi forget','led on','ask /help'):
  assert 'model is not configured' in local(text,expected='invalid')
 local(restore,True);assert cfg['model'] in local('/model')
 quote=lambda v:json.dumps(v,ensure_ascii=False)
 local('/wifi set '+quote(cfg['ssid'])+' '+quote(cfg['password']),True)
 connected=False
 for attempt in range(2):
  local('/wifi connect')
  for _ in range(15):
   time.sleep(1);out=local('/wifi status')
   if 'ipv4=' in out and 'ipv4=0.0.0.0' not in out:connected=True;break
   if 'SDK reason=202' in out:break
  if connected:break
 assert connected,'Wi-Fi failed after bounded requests'
 for c in ('/led solid 0 255 0','/led blink 255 0 0 1000','/led breathe 0 0 255 2000','/led rainbow 3000','/led off','/led on','/led off'):
  assert 'applied:' in local(c)
 local('/led solid 256 0 0',expected='invalid');local('/led rainbow 1',expected='invalid')
 # Keep local operations responsive while the real model request is pending.
 start,job,_=begin('关闭板载灯，然后分两行报告实际结果和设备类型。')
 wait(rb'model task queued; actual tool results',start)
 assert '[模型]' not in local('/help')
 status=local('/model status');assert 'task=running' in status,status
 local('/led on',expected='busy')
 wait(re.escape('[任务结束] status=observed').encode(),start,90)
 output=bytes(rx[start:]).decode('utf8','replace');assert 'applied:' in output and '\\x0a' not in output
 print('\nREAL MODEL / CONCURRENT LOCAL HELP\n'+safe(output),flush=True)
 for prompt,on in (('把灯改成蓝色呼吸，周期2秒。',1),('那关掉呢？',0),('再打开，保持刚才的效果。',1),('关闭板载灯。',0)):
  start,job,_=begin(prompt);wait(rb'model task queued; actual tool results',start)
  wait(re.escape('[任务结束] status=observed').encode(),start,90)
  output=bytes(rx[start:]).decode('utf8','replace');assert ('applied: pin=48 on='+str(on)+' mode=breathe rgb=0,0,255 period_ms=2000') in output,safe(output)
  print('\nNO-ASK ROUND '+prompt+'\n'+safe(output),flush=True)
 local('/led off');local('/led status');local('/status')
 Path('reports/slash-shell/live-cases.json').write_text(json.dumps(results,ensure_ascii=False,indent=2))
 print('PASS local/offline help, model set/status/forget, private Wi-Fi alias, direct LED limits, busy guard and no-prefix real Qwen',flush=True)
finally:
 # Restore a usable real-model endpoint even if a local test fails.
 try:local(restore,True);local('/led off')
 finally:s.close()
