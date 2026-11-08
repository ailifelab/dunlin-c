#!/usr/bin/env python3
"""Host regression only; uses prepared pinned sources, no serial/network access."""
import os
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parent.parent
os.chdir(root)
out = root / 'build/micropython-host-regression'
out.mkdir(parents=True, exist_ok=True)
cc = os.environ.get('CC', 'cc')
json_dir = root / 'build/pio-core/packages/framework-espidf/components/json/cJSON'
def run(args):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True)
def agent(name, source, extra=()):
    binary = out / name
    run([cc, '-std=c11', '-O1', '-g', *extra, '-I'+str(json_dir), source,
         'dunlin-copilot/esp32/agent/agent.c', json_dir/'cJSON.c', '-lm', '-o', binary])
    run([binary])
for name, source in [('policy','tests/embedded/agent.c'),
                     ('alloc','tests/embedded/agent_alloc.c'),
                     ('python-policy','tests/embedded/agent_python.c')]:
    agent(name, source, ['-fsanitize=address,undefined','-fno-omit-frame-pointer'])
agent('disabled','tests/embedded/agent_python_disabled.c',['-DAE_AGENT_ENABLE_PYTHON=0'])
run(['python3', '-B', 'tests/test_device_config.py'])
run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', 'dunlin-copilot/esp32/common/http_date.c',
     'tests/embedded/http_date.c', '-o', out/'date'])
run([out/'date'])
component = root/'dunlin-copilot/esp32/esp32/components/micropython'
generated = component/'generated'
# Conservative GC stack scanning is intentional; this executable is not ASan evidence.
run([cc, '-std=gnu11', '-O1', '-g', '-I'+str(component), '-I'+str(generated),
     '-I'+str(generated/'port'), *sorted((generated/'py').glob('*.c')),
     generated/'shared/runtime/gchelper_generic.c', generated/'extmod/modjson.c',
     component/'mpport.c', 'dunlin-copilot/esp32/python/mpbridge.c', 'tests/embedded/python_vm.c',
     '-lm', '-o', out/'vm'])
run([out/'vm'])
print('PASS host subset; weather transport is a fixture, not real Qwen/hardware evidence')
