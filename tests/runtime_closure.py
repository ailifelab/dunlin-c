"""Run the Linux product in a minimal, unprivileged chroot inside the test container.
Only the ELF and its loaded libraries go inside. The test server stays outside.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path
from mock_model import MockServer

binary=Path(sys.argv[1]).resolve()
libs=re.findall(r'(?:=> )?(/[^\s]+) \(0x',subprocess.check_output(['ldd',str(binary)]).decode())
assert libs and any('ld-linux' in p for p in libs), libs
assert not any(re.search('python|libnode|Qt|libstdc',p) for p in libs),libs
server=MockServer()
threading.Thread(target=server.serve_forever,daemon=True).start()
try:
    with tempfile.TemporaryDirectory() as d:
        root=Path(d)
        os.chmod(d,0o755)
        for lib in libs:
            dest=root / lib.lstrip('/')
            dest.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(lib,str(dest))
            dest.chmod(0o755)
        shutil.copy2(str(binary),str(root/'dunlin'))
        for name in ['work','sessions']:
            folder=root/name
            folder.mkdir(mode=0o700)
            os.chown(str(folder),65534,65534)
        cfg=dict(base_url='http://127.0.0.1:{}/v1'.format(server.server_port),model='mock-local',stream=True)
        (root/'config.json').write_text(json.dumps(cfg))
        prefix=['chroot','--userspec=65534:65534',d,'/dunlin','--session','closure','--config','/config.json','--json']
        for cmd,reply in [(['run','multiple'],'results=你好|42'),(['resume','remember'],'history=multiple')]:
            output=subprocess.check_output(prefix+cmd,env={'PATH':'/usr/sbin:/usr/bin:/bin','LC_ALL':'C'},timeout=15)
            obj=json.loads(output.decode('utf-8'))
            assert obj['status']=='completed' and obj['text']==reply,obj
        print('PASS clean runtime, UID 65534. Executables: /dunlin and ELF loader only.')
        print('PASS streamed echo+calc, paired results, process restart and follow-up inside chroot')
        print('runtime files: '+', '.join(sorted(libs)))
finally:
    server.shutdown()
    server.server_close()
