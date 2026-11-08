#!/usr/bin/env python3
"""Explicit ordinary-ESP32 serial acceptance. No GPIO writes or credentials.
Only run with exclusive ownership of the selected board. Model config stays RAM-only.
"""
import argparse
import re
import time
import termios
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    args = parser.parse_args()
    connection = serial.Serial(port=None, baudrate=115200, timeout=.05, exclusive=True)
    connection.dtr = False
    connection.rts = False
    connection.port = args.port
    with connection:
        settings = termios.tcgetattr(connection.fileno())
        settings[2] &= ~termios.HUPCL
        termios.tcsetattr(connection.fileno(), termios.TCSANOW, settings)
        def read_until(pattern, timeout=8):
            data = bytearray()
            until = time.monotonic() + timeout
            while time.monotonic() < until:
                data.extend(connection.read(4096))
                match = re.search(pattern, data)
                if match:
                    text = data.decode('utf8', 'replace')
                    assert 'Guru Meditation' not in text and 'Brownout' not in text, text
                    print(text, flush=True)
                    return match, text
            raise AssertionError('serial timeout: ' + data.decode('utf8', 'replace'))
        def command(line, status='ok', contains=None):
            print('COMMAND ' + line, flush=True)
            connection.write((line + '\n').encode())
            match, text = read_until(rb'(?:^|[\r\n])job=(\d+) (ok|invalid|denied|unsupported|busy|io_error|limit):[^\r\n]*\r?\n')
            assert match[2].decode() == status, text
            assert 'queued job=' + match[1].decode() in text, text
            if contains:
                assert contains in text, text
            return text
        # CP2102 open may transiently reset the board despite pre-set line states.
        startup = bytearray()
        until = time.monotonic() + 1.5
        while time.monotonic() < until:
            startup.extend(connection.read(4096))
        print(startup.decode('utf8', 'replace'), flush=True)
        command('/help', contains='local help shown')
        command('/status', contains='heap_free=')
        command('/plugins', contains='embedded-io active')
        command('/gpio write 18 1', 'denied')
        command('/io allow 6', 'denied')
        command('/wifi status')
        command('/bt status')
        command('/model status', contains='storage=NVS')
        command('/led pin 48', 'unsupported')
        command('/not-a-command', 'invalid')
        print('COMMAND oversized line (400 bytes)', flush=True)
        connection.write(b'x' * 400 + b'\n')
        read_until(rb'(?:limit|invalid):[^\r\n]*\r?\n')
        command('/status', contains='heap_free=')
        print('PASS ordinary ESP32 serial acceptance; no physical IO or model call', flush=True)


if __name__ == '__main__':
    main()
