"""Physical S3 owner/responsiveness checks; requires exclusive serial access."""
import re, time, termios, serial
s=serial.Serial(port=None, baudrate=115200, timeout=.02, exclusive=True)
s.dtr=True; s.rts=True; s.port='/dev/cu.usbmodem2101'; s.open()
t=termios.tcgetattr(s.fileno()); t[2]&=~termios.HUPCL
termios.tcsetattr(s.fileno(),termios.TCSANOW,t)
rx=bytearray()
def wait(pattern, timeout=10):
    until=time.monotonic()+timeout
    while time.monotonic()<until:
        if re.search(pattern,rx):return
        rx.extend(s.read(4096))
    raise AssertionError(bytes(rx).decode('utf8','replace'))
def send(text):
    print('SEND '+text,flush=True); s.write((text+'\n').encode());s.flush()
try:
    send('/python run while True: pass')
    wait(rb'Python task queued')
    send('/python status'); send('/help python')
    wait(rb'task=busy'); wait('模块json'.encode())
    assert '[Python结束]'.encode() not in rx, 'status/help did not finish while VM was active'
    wait(rb'\[Python'+re.escape('结束'.encode())+rb'\] status=failed')
    assert b'Python execution timeout' in rx
    send('/python run print(dunlin.led("solid",0,32,0,1000))')
    wait(rb'pin=48 on=1 mode=solid rgb=0,32,0 period_ms=1000')
    wait(rb'\[Python'+re.escape('结束'.encode())+rb'\] status=ok')
    send('/led off'); wait(rb'pin=48 on=0 mode=solid')
    assert b'ESP-ROM:' not in rx
    print(rx.decode('utf8','replace'),flush=True)
    print('PASS status/help during active VM, timeout recovery, native Python existing LED driver, final off; no runtime reboot observed')
finally:s.close()
