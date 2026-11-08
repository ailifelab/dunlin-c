"""Physical S3 cloud benchmark. Credentials stay private; no model shell approvals."""
import serial,termios,time,re,json,uuid
from weather_evidence import verify_weather
from pathlib import Path
report=Path('reports/openai-sol')/('device-'+uuid.uuid4().hex[:8]);report.mkdir()
key=Path('dunlin-trial/openai-sol/api-key').read_text().strip();old=json.loads(Path('dunlin-trial/esp32-live.json').read_text());secrets=[key,old['key'],old.get('password','')]
s=serial.Serial(port=None,baudrate=115200,timeout=.05,exclusive=True);s.dtr=True;s.rts=True;s.port='/dev/cu.usbmodem2101';s.open();t=termios.tcgetattr(s.fileno());t[2]&=~termios.HUPCL;termios.tcsetattr(s.fileno(),termios.TCSANOW,t)
rx=bytearray();rows=[];sequence=0

def clean(text):
 for secret in secrets:
  if secret:text=text.replace(secret,'[REDACTED]')
 return text

def wait(pattern,start,timeout=150):
 until=time.monotonic()+timeout
 while time.monotonic()<until:
  found=re.search(pattern,bytes(rx[start:]))
  if found:return found
  rx.extend(s.read(4096))
 raise TimeoutError(clean(bytes(rx[start:]).decode('utf8','replace')))

def command(text,secret=False):
 global sequence
 sequence+=1;start=len(rx);begin=time.monotonic();s.write((text+'\n').encode());s.flush();queued=wait(rb'queued job=(\d+)',start,15).group(1)
 done=wait(rb'(?:^|[\r\n])job='+queued+rb' (\w+):[^\r\n]*\r?\n',start,15)
 out=bytes(rx[start:]).decode('utf8','replace')
 if 'model task queued' in out:wait(rb'(?:^|[\r\n])\['+re.escape('任务结束'.encode())+rb'\] status=\w+;[^\r\n]*\r?\n',start)
 if 'Python task queued' in out:wait(rb'(?:^|[\r\n])\['+re.escape('Python结束'.encode())+rb'\] status=\w+\r?\n',start)
 elapsed=time.monotonic()-begin;out=clean(bytes(rx[start:]).decode('utf8','replace'));label='/model set [private]' if secret else text
 (report/(str(sequence)+'.log')).write_text('COMMAND '+label+'\n'+out)
 assert done.group(1)==b'ok',out
 assert 'ESP-ROM:' not in out,'unexpected runtime reboot'
 return out,round(elapsed,3)

ok=False
try:
 command('/model status');command('/wifi status');command('/python status')
 for effort in __import__('os').environ.get('DUNLIN_SOL_EFFORT_ORDER','low,xhigh').split(','):
  command('/model time '+str(int(time.time())))
  command('/model set https://api.openai.com/v1/responses gpt-6.1-sol '+key,True)
  command('/effort '+effort);command('/model status')
  command('/led solid 32 32 32');command('/led off')
  cases=[('hello','你好',None),('python','请使用python_exec工具在MicroPython中计算1到100的整数和，打印实际运行结果。',None),('blue','请把板载灯设为蓝色呼吸，周期2秒。',1),('off','那关掉呢？',0),('restore','再打开，保持刚才的效果。',1),('weather','请使用MicroPython查询上海现在的天气，报告ISO格式数据时间、摄氏温度和来源。',None)]
  led_ready=False
  for name,prompt,on in cases:
   if name in ('off','restore') and not led_ready:
    rows.append({'effort':effort,'case':name,'status':'unverified','reason':'LED precondition failed'});continue
   out,elapsed=command(prompt);passed=False
   if name=='hello':passed='status=replied' in out and 'applied:' not in out
   elif name=='python':passed='python_exec' in out and '| 5050' in out and 'status=observed' in out
   elif on is not None:
    passed=('on='+str(on)+' mode=breathe rgb=0,0,255 period_ms=2000') in out and 'status=observed' in out
    led_ready=passed
   else:
    try:verify_weather(out);passed=True
    except AssertionError:passed=False
   metrics=re.search(r'\[OpenAI metrics\] ([^\r\n]+)',out)
   row={'effort':effort,'case':name,'status':'pass' if passed else 'fail','elapsed_s':elapsed,'metrics':metrics.group(1) if metrics else None,'log':str(report/(str(sequence)+'.log'))};rows.append(row)
   (report/'results.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2)+'\n');print(json.dumps(row,ensure_ascii=False),flush=True)
  command('/led off');command('/status')
 ok=all(r['status']=='pass' for r in rows)
 if ok:
  command('/model set https://api.openai.com/v1/responses gpt-6.1-sol '+key,True);command('/model effort low');command('/model status')
 print('ALL CASES PASSED='+str(ok)+'; report='+str(report),flush=True)
finally:
 if not ok:
  try:command('/model set '+old['url']+' '+old['model']+' '+(old['key'] or '-'),True)
  except Exception as exc:print('Restore previous model failed:',clean(str(exc)),flush=True)
 try:command('/led off')
 finally:s.close()
raise SystemExit(0 if ok else 1)
