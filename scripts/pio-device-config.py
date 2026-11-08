"""S3 build hook: compile configuration sources into Flash constants."""
Import('env')
from pathlib import Path
import subprocess,sys
root=next(p for p in Path(env['PROJECT_DIR']).resolve().parents if (p/'CURRENT-VERSION.json').is_file())
command=[sys.executable,'-B',str(root/'scripts/generate-device-config.py')]
if env['PIOENV']!='esp32s3':command+=['--prompt-only']
subprocess.run(command,check=True)
