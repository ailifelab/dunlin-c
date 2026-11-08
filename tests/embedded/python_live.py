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
 out=command('/python status');assert 'available=yes' in out and 'PSRAM=8388608' in out
 assert 'local help' in command('/help python')
 model_status=command('/model status');assert 'configured=yes' in model_status
 active_model=re.search(r'(?:^|[\r\n])(?:> )?model=([^\r\n]+)',model_status).group(1)
 for code,needle,status in [('print(1+2)','| 3','ok'),('while True: pass','Python execution timeout','failed'),('print("recovered")','recovered','ok'),('x="x"*2000000','MemoryError','failed'),('raise ValueError("test")','ValueError','failed'),('import os','ImportError','failed'),('import dunlin; print(dunlin.http_get("https://example.com/"))','host not permitted','failed'),('print("x"*2000)','limit exceeded','failed'),('print("\\n"*100)','limit exceeded','failed'),('print(6*7)','| 42','ok')]:
  out=command('/python run '+code);assert needle in out and '[Python结束] status='+status in out,out
 out=command('你现在使用的是哪个模型？');assert re.search(r'(?:^|[\r\n])\[任务结束\] status=(?:replied|observed); model='+re.escape(active_model)+r';',out),out
 out=command('请用MicroPython计算1到100整数的和，报告实际运行结果。');assert 'python_exec' in out and '5050' in out and '[任务结束] status=observed' in out
 out=command('帮我查一下上海现在的天气，告诉我数据时间、温度和来源。');assert '[Python/HTTPS] 正在连接已授权主机：api.open-meteo.com' in out and out.count('HTTP 200')>=2 and '[任务结束] status=observed' in out,out
 verify_weather(out)
 Path(os.environ.get('DUNLIN_WEATHER_EVIDENCE','reports/esp32-micropython/weather-real-result.txt')).write_text(out)
 for text,on in [('把灯改成蓝色呼吸，周期2秒。',1),('那关掉呢？',0),('再打开，保持刚才的效果。',1),('关闭板载灯。',0)]:
  out=command(text);assert ('on='+str(on)+' mode=breathe rgb=0,0,255 period_ms=2000') in out and '[任务结束] status=observed' in out,out
 command('/status');command('/wifi status');command('/model status')
 print('PASS physical PSRAM, generic Python/negative recovery, active model identity, real Qwen Python arithmetic/weather and exact LED follow-ups; NVS never reprovisioned',flush=True)
finally:
 try:command('/led off')
 finally:s.close()
