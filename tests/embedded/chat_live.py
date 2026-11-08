"""Physical S3 plain conversation regression. Does not provision or change IO."""
import serial,time,re,termios,sys
s=serial.Serial(port=None,baudrate=115200,timeout=.05,exclusive=True);s.dtr=True;s.rts=True;s.port='/dev/cu.usbmodem2101';s.open();t=termios.tcgetattr(s.fileno());t[2]&=~termios.HUPCL;termios.tcsetattr(s.fileno(),termios.TCSANOW,t)
def command(text):
 data=bytearray();s.write((text+'\n').encode());s.flush();end=time.monotonic()+140
 while time.monotonic()<end:
  data.extend(s.read(4096))
  if re.search(rb'(?:^|[\r\n])\['+re.escape('任务结束'.encode())+rb'\] status=\w+;[^\r\n]*\r?\n',data):break
 else:raise TimeoutError(data.decode('utf8','replace'))
 out=data.decode('utf8','replace');print('COMMAND '+text+'\n'+out,flush=True);assert 'ESP-ROM:' not in out;return out
try:
 if '--before' in sys.argv:
  out=command('你好');assert 'status=stopped' in out;print('REPRODUCED actual Qwen greeting incorrectly stopped; user failure confirmed')
 else:
  for text in ['你好','谢谢你','解释一下什么是二进制，简短回答。']:
   out=command(text);assert 'status=replied' in out and '任务停止' not in out and 'applied:' not in out and '[工具]' not in out,out
  print('PASS actual Qwen greeting, thanks and knowledge reply without tool effects or false observed status')
finally:s.close()
