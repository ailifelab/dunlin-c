#!/usr/bin/env python3
"""Launch owned GUI test instance using existing local-service credential reference.
Never copies the secret to settings, evidence, command arguments or Keychain.
"""
import os,pathlib,subprocess
root=pathlib.Path(__file__).resolve().parents[1];env=os.environ.copy()
for line in (root/'dunlin-trial/model.env').read_text().splitlines():
 if line.startswith('OMLX_API_KEY='):env['OMLX_API_KEY']=line.split('=',1)[1]
with (root/'reports/desktop-live-20261107/ui-final.log').open('a') as log:
 p=subprocess.Popen([str(root/'dist/macos/Dunlin C.app/Contents/MacOS/DunlinApp'),'--data-dir',str(root/'build/desktop-live/ui-final')],env=env,stdout=log,stderr=subprocess.STDOUT)
 (root/'build/desktop-live/app.pid').write_text(str(p.pid));print('owned GUI pid',p.pid,flush=True);p.wait()
