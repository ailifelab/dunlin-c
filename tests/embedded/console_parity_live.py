#!/usr/bin/env python3
"""Authorized ESP32 console/NVS regression. Exclusive serial; no GPIO effects.
Full mode temporarily forgets each profile, restores private configuration, hard
resets, and calls the existing model. --check-only observes the installed contract.
"""
import argparse
import json
import re
import termios
import time
from pathlib import Path
import serial


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--private-config')
    p.add_argument('--check-only', action='store_true')
    a = p.parse_args()
    config = json.loads(Path(a.private_config).read_text()) if a.private_config else {}
    def safe(text):
        for field in ('password', 'key'):
            if config.get(field):
                text = text.replace(config[field], '[REDACTED]')
        return text
    s = serial.Serial(port=None, baudrate=115200, timeout=.05, exclusive=True)
    s.dtr = False
    s.rts = False
    s.port = a.port
    with s:
        flags = termios.tcgetattr(s.fileno())
        flags[2] &= ~termios.HUPCL
        termios.tcsetattr(s.fileno(), termios.TCSANOW, flags)
        captured = []
        def drain(seconds):
            data = bytearray()
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                data.extend(s.read(4096))
            text = safe(data.decode('utf8', 'replace'))
            captured.append(text)
            if text:
                print(text, flush=True)
            assert 'Guru Meditation' not in text and 'Brownout' not in text, text
            return text
        def wait(pattern, timeout=12):
            data = bytearray()
            until = time.monotonic() + timeout
            while time.monotonic() < until:
                data.extend(s.read(4096))
                match = re.search(pattern, data)
                if match:
                    text = safe(data.decode('utf8', 'replace'))
                    captured.append(text)
                    print(text, flush=True)
                    assert 'Guru Meditation' not in text, text
                    return match, text
            raise AssertionError('serial timeout: ' + safe(data.decode('utf8', 'replace')))
        def command(line, status='ok', contains=None, private=False):
            print('COMMAND ' + ('[private config]' if private else line), flush=True)
            s.write((line + '\n').encode())
            match, text = wait(rb'(?:^|[\r\n])job=(\d+) (ok|invalid|denied|unsupported|busy|io_error|limit):[^\r\n]*\r?\n')
            assert match[2].decode() == status, text
            if contains:
                assert contains in text, text
            return text
        def reboot():
            print('HARD RESET; NO HOST RECONFIGURATION', flush=True)
            s.dtr = False
            s.rts = True
            time.sleep(.15)
            s.rts = False
            text = drain(3)
            assert 'DUNLIN FreeRTOS serial ABI 1 (esp32)' in text, text
            return text
        def wifi_ready():
            for _ in range(25):
                text = command('/wifi status')
                if 'ipv4=' in text and 'ipv4=0.0.0.0' not in text:
                    return
                time.sleep(1)
            raise AssertionError('Wi-Fi did not automatically acquire IP')
        def save_model():
            command('/model set ' + config['url'] + ' ' + config['model'] + ' ' + (config.get('key') or '-'), contains='saved to NVS', private=True)
        def save_wifi():
            command('/wifi set ' + json.dumps(config['ssid'], ensure_ascii=False) + ' ' + json.dumps(config['password'], ensure_ascii=False), contains='restored on reboot', private=True)
        def chat(prompt, device=False):
            start = len(captured)
            command(prompt, contains='model task queued')
            command('/help', contains='local help shown')
            command('/status', contains='config=NVS+RAM')
            if not re.search(r'\[任务结束\] status=(?:replied|observed);', ''.join(captured[start:])):
                wait(re.escape('[任务结束] status=').encode() + rb'(?:replied|observed);[^\r\n]*\r?\n', 140)
            text = ''.join(captured[start:])
            if device:
                assert '"device":"esp32"' in text and 'status=observed' in text, text
            assert '\\x0a' not in text, text
        drain(2)
        command('/status', contains='config=NVS+RAM')
        if a.check_only:
            print('PASS installed slash console/NVS status contract', flush=True)
            return
        assert config, 'full test requires authorized private config'
        for line in ('/help', '/h', '/?', '/帮助', '/help wifi', '/help model'):
            command(line, contains='local help shown')
        command('/unknown-parity-command', status='invalid', contains='/help')
        command('/python status', status='unsupported')
        command('/gpio write 18 1', status='denied')
        save_wifi()
        save_model()
        boot = reboot()
        assert '已恢复模型配置' in boot and '已恢复Wi-Fi配置' in boot, boot
        command('/model', contains=config['model'])
        wifi_ready()
        chat('你好，请用一句中文回复。')
        command('/model set https://invalid.example fixture -', status='invalid')
        command('/models', contains=config['model'])
        command('/model forget')
        reboot()
        command('/model', contains='configured=no')
        wifi_ready()
        for text in ('你好', 'help', 'status', 'ask 你好', 'wifi forget'):
            command(text, status='invalid', contains='model is not configured')
        save_model()
        command('/wifi forget')
        reboot()
        command('/wifi', contains='WiFi disabled')
        command('/model', contains=config['model'])
        save_wifi()
        boot = reboot()
        assert '已恢复模型配置' in boot and '已恢复Wi-Fi配置' in boot, boot
        wifi_ready()
        chat('请调用device_info报告设备类型和当前模型，不要操作灯。', device=True)
        command('/model status', contains='storage=NVS')
        command('/status', contains='config=NVS+RAM')
        print('PASS ordinary ESP32 shared S3 console; independent NVS forget/restore, repeated hard resets and direct real-model chat without host reconfiguration', flush=True)


if __name__ == '__main__':
    main()
