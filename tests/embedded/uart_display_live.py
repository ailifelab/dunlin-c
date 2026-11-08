#!/usr/bin/env python3
"""Capture complete raw UART and decoded replies. No private config or IO commands.
Use only while this task exclusively owns the port; persists raw evidence at --output.
"""
import argparse
import json
import re
import termios
import time
from pathlib import Path
import serial
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--output',required=True);a=p.parse_args()
s=serial.Serial(port=None,baudrate=115200,timeout=.05,exclusive=True);s.dtr=False;s.rts=False;s.port=a.port
raw=bytearray();cases=[]
with s:
    flags=termios.tcgetattr(s.fileno());flags[2]&=~termios.HUPCL;termios.tcsetattr(s.fileno(),termios.TCSANOW,flags)
    def drain(seconds):
        until=time.monotonic()+seconds
        while time.monotonic()<until:raw.extend(s.read(4096))
    drain(3)
    try:
        for prompt in ['/help','你好','今天日期是什么，是星期几？','能查到日期吗','/status']:
            start=len(raw);s.write((prompt+'\n').encode());s.flush();deadline=time.monotonic()+140
            pattern=rb'job=\d+ ok:[^\r\n]*\r?\n' if prompt.startswith('/') else re.escape('[任务结束] status=').encode()+rb'(?:replied|observed|stopped);[^\r\n]*\r?\n'
            while not re.search(pattern,raw[start:]):
                assert time.monotonic()<deadline,'completion timeout'
                raw.extend(s.read(4096))
            drain(.2)
            text=bytes(raw[start:]).decode('utf8',errors='strict')
            assert '(null)' not in text and 'Guru Meditation' not in text and '\x00' not in text,text
            if not prompt.startswith('/'):
                assert 'status=stopped' not in text,text
                assert '串口仍可接收命令。' in text,text
            cases.append({'prompt':prompt,'text':text});print('COMMAND '+prompt+'\n'+text,flush=True)
    finally:
        Path(a.output+'.bin').write_bytes(raw)
        Path(a.output+'.json').write_text(json.dumps(cases,ensure_ascii=False,indent=2)+'\n')
print('PASS raw UTF8/complete task footer/no literal (null); date reply is not clock evidence')
