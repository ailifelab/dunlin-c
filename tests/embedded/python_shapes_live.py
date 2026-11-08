"""Physical VM exception shapes; no network or hardware effects."""
import serial,termios,time,re
s=serial.Serial(port=None,baudrate=115200,timeout=.05,exclusive=True);s.dtr=True;s.rts=True;s.port='/dev/cu.usbmodem2101';s.open();t=termios.tcgetattr(s.fileno());t[2]&=~termios.HUPCL;termios.tcsetattr(s.fileno(),termios.TCSANOW,t)
try:
 s.write(b'/python run d={"results":[{"name":"Shanghai","latitude":31}]}; print(d["results"][0]["geonym"])\n');s.flush();rx=bytearray();end=time.monotonic()+12
 while time.monotonic()<end:
  rx.extend(s.read(4096))
  if '[Python结束] status=failed'.encode() in rx:break
 out=rx.decode('utf8','replace');print(out,flush=True);assert 'KeyError: geonym' in out
 assert 'd.results[0] keys:' in out and 'name' in out and 'latitude' in out
 assert 'Shanghai' not in out, 'diagnostics must not expose dictionary values'
 print('PASS bounded nested dictionary/list shape diagnostics expose keys without values')
finally:s.close()
