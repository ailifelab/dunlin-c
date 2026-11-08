"""Physical S3 regression; no provisioning or secrets sent to the serial log."""
import serial,time,re,termios,json,os
from pathlib import Path
from weather_evidence import verify_weather
s=serial.Serial(port=None,baudrate=115200,timeout=.05,exclusive=True);s.dtr=True;s.rts=True;s.port='/dev/cu.usbmodem2101';s.open();t=termios.tcgetattr(s.fileno());t[2]&=~termios.HUPCL;termios.tcsetattr(s.fileno(),termios.TCSANOW,t)
rx=bytearray();checks=[]
def wait(pattern,start,timeout=140):
 end=time.monotonic()+timeout
 while True:
  m=re.search(pattern,bytes(rx[start:]))
  if m:return m
  if time.monotonic()>end:raise TimeoutError(bytes(rx[start:]).decode('utf8','replace'))
  rx.extend(s.read(4096))
def command(text,expected='ok'):
 start=len(rx);s.write((text+'\n').encode());s.flush();q=wait(rb'queued job=(\d+)',start);job=q.group(1)
 m=wait(rb'(?:^|[\r\n])job='+job+rb' (\w+):[^\r\n]*\r?\n',start);assert m.group(1).decode()==expected,bytes(rx[start:])
 out=bytes(rx[start:]).decode('utf8','replace')
 if 'Python task queued' in out:wait(re.escape('[Python结束] status=').encode()+rb'[^\r\n]*\r?\n',start)
 if 'model task queued' in out:wait(re.escape('[任务结束] status=').encode()+rb'[^\r\n]*\r?\n',start)
 out=bytes(rx[start:]).decode('utf8','replace');assert 'ESP-ROM:' not in out,'unexpected reboot'
 print('\nCOMMAND '+text+'\n'+out,flush=True);return out
try:
 for text,on in [('把灯改成蓝色呼吸，周期2秒。',1),('那关掉呢？',0),('再打开，保持刚才的效果。',1),('关闭板载灯。',0)]:
  out=command(text);assert ('on='+str(on)+' mode=breathe rgb=0,0,255 period_ms=2000') in out and '[任务结束] status=observed' in out,out
 print('PASS actual Qwen LED blue/off/on exact restore follow-ups',flush=True)
finally:
 try:command('/led off')
 finally:s.close()
