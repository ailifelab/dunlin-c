"""Read-only console verification after an externally requested board reset."""
import serial,termios,time,re
s=serial.Serial(port=None,baudrate=115200,timeout=.05,exclusive=True);s.dtr=True;s.rts=True;s.port='/dev/cu.usbmodem2101';s.open();t=termios.tcgetattr(s.fileno());t[2]&=~termios.HUPCL;termios.tcsetattr(s.fileno(),termios.TCSANOW,t)
try:
 for cmd,expected in [('/python status','available=yes'),('/model status','storage=NVS'),('/wifi status',r'WiFi associated; ipv4=(?!0\.0\.0\.0)\d+\.\d+\.\d+\.\d+'),('/led status','on=0'),('/status','config=NVS\+RAM')]:
  data=bytearray();s.write((cmd+'\n').encode());s.flush();end=time.monotonic()+12
  while time.monotonic()<end:
   data.extend(s.read(4096));m=re.search(rb'(?:^|[\r\n])job=\d+ ok:[^\r\n]*\r?\n',data)
   if m:break
  text=data.decode('utf8','replace');print('COMMAND '+cmd+'\n'+text,flush=True);assert re.search(expected,text),text
 print('PASS final reset restores profile, Wi-Fi, VM; startup LED off; no provisioning commands sent')
finally:s.close()
