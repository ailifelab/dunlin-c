#!/usr/bin/env python3
"""Explicit serial commands and optional private device configuration, no auto flash.
Run with the existing PlatformIO venv (pyserial). Private JSON is never logged.
"""
import argparse,json,re,time,termios,sys
import serial
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--port',required=True);p.add_argument('--target',choices=('esp32s3','esp32'),default='esp32s3');p.add_argument('--private-config');p.add_argument('--configure',action='store_true');p.add_argument('--command',action='append',default=[]);p.add_argument('--wait',type=float,default=140);p.add_argument('--interactive',action='store_true');p.add_argument('--status-during-agent',action='store_true')
a=p.parse_args();private=json.loads(Path(a.private_config).read_text()) if a.private_config else {};secrets=[private.get('password',''),private.get('key','')]
def display(data):
    text=data.decode('utf-8',errors='replace')
    for secret in secrets:
        if secret:text=text.replace(secret,'[REDACTED]')
    print(text,end='',flush=True)
s=serial.Serial(port=None,baudrate=115200,timeout=.1);s.dtr=a.target=='esp32s3';s.rts=a.target=='esp32s3';s.port=a.port;s.exclusive=True;s.open()
settings=termios.tcgetattr(s.fileno());settings[2]&=~termios.HUPCL;termios.tcsetattr(s.fileno(),termios.TCSANOW,settings)
def drain(seconds):
    data=bytearray();end=time.monotonic()+seconds
    while time.monotonic()<end:
        chunk=s.read(4096)
        if chunk: data.extend(chunk)
    if data: display(data)
    return bytes(data)
def command(line,private_line=False):
    print('\n[SEND] '+('[private device config]' if private_line else line),flush=True)
    s.write((line+'\n').encode());s.flush();data=bytearray();started=time.monotonic();deadline=started+a.wait;health_sent=False
    while time.monotonic()<deadline:
        if a.status_during_agent and (line.startswith(('/ask ','/ask-blind ','ask ','ask-blind ')) or b'model task queued; actual tool results' in data) and not health_sent and time.monotonic()-started>1:
            print('\n[HEALTH] status while model task is pending',flush=True);s.write(b'/status\n');s.flush();health_sent=True
        chunk=s.read(4096)
        if not chunk:continue
        data.extend(chunk);display(chunk)
        if re.search(rb'(?:^|[\r\n])job=\d+ ok: Python task queued; completion follows[^\r\n]*\r?\n',data):
            if re.search(('(?:^|[\r\n])'+re.escape('[Python结束] status=')).encode()+rb'[^\r\n]*\r?\n',data):return bytes(data)
        elif line.startswith(('/ask ','/ask-blind ','ask ','ask-blind ')) or re.search(rb'(?:^|[\r\n])job=\d+ ok: model task queued; actual tool results[^\r\n]*\r?\n',data):
            if re.search(rb'(?:^|[\r\n])job=\d+ (ok|invalid): local help shown; no model request\r?\n',data):return bytes(data)
            if re.search(('(?:^|[\r\n])'+re.escape('[任务结束] status=')).encode()+rb'[^\r\n]*\r?\n',data):return bytes(data)
            if re.search(rb'job=\d+ (invalid|busy|limit|io_error):',data):raise RuntimeError('agent request rejected')
        elif re.search(rb'(?:^|[\r\n])job=\d+ (ok|invalid|denied|unsupported|busy|io_error|limit):[^\r\n]*\r?\n',data):return bytes(data)
    raise TimeoutError('serial completion not observed')
try:
    drain(1)
    if a.configure:
        quote=lambda s:json.dumps(s,ensure_ascii=False)
        result=command('/wifi set '+quote(private['ssid'])+' '+quote(private['password']),True)
        if b' ok:' not in result:raise RuntimeError('Wi-Fi configuration rejected')
        connected=False
        for attempt in range(2):
            command('/wifi connect')
            for _ in range(15):
                time.sleep(1);result=command('/wifi status')
                if b'ipv4=' in result and b'ipv4=0.0.0.0' not in result:connected=True;break
                if b'SDK reason=202' in result:break
            if connected:break
        if not connected:raise RuntimeError('Wi-Fi address not acquired after two bounded connect requests; inspect SDK reason')
        result=command('/agent set '+private['url']+' '+private['model']+' '+(private.get('key') or '-'),True)
        if b' ok:' not in result:raise RuntimeError('Model configuration rejected')
    for line in a.command:command(line);drain(.2)
    if a.interactive:
        print('[HOST] Serial attached; enter device commands, /quit to detach.',flush=True)
        for line in sys.stdin:
            line=line.rstrip('\r\n')
            if line=='/quit':break
            if line=='/model-config':
                command('/agent set '+private['url']+' '+private['model']+' '+(private.get('key') or '-'),True)
            elif line:command(line);drain(.2)
finally:s.close()
