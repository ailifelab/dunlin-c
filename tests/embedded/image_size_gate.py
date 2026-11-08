#!/usr/bin/env python3
"""Exercise the actual PlatformIO post-action with the pinned SDK and size edges."""
import runpy
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / 'build/pio-core/packages/framework-espidf'
sys.path.insert(0, str(SDK / 'components/partition_table'))
from gen_esp32part import PartitionTable

table = PartitionTable.from_csv((ROOT / 'dunlin-copilot/esp32/esp32/partitions-esp32.csv').read_text())
table.verify()
parts = {p.name: p for p in table}
assert (parts['nvs'].offset, parts['nvs'].size) == (0x9000, 0x6000)
assert (parts['phy_init'].offset, parts['phy_init'].size) == (0xf000, 0x1000)
assert (parts['factory'].offset, parts['factory'].size) == (0x10000, 0x200000)
assert max(p.offset + p.size for p in table) <= 0x400000

with tempfile.TemporaryDirectory(prefix='esp32-image-gate-') as folder:
    build = Path(folder)
    (build / 'partitions.bin').write_bytes(table.to_binary())
    binary = build / 'firmware.bin'

    class Environment:
        def PioPlatform(self):
            return self

        def get_package_dir(self, name):
            assert name == 'framework-espidf'
            return str(SDK)

        def subst(self, key):
            return {'$BUILD_DIR': str(build), '$PYTHONEXE': sys.executable}[key]

        def AddPostAction(self, name, action):
            assert name == '$BUILD_DIR/${PROGNAME}.bin'
            self.action = action

    env = Environment()
    runpy.run_path(str(ROOT / 'scripts/pio-check-device-size.py'),
                   init_globals={'env': env, 'Import': lambda name: None})
    with binary.open('wb') as stream:
        stream.truncate(0x200000)
    env.action(source=[], target=[binary], env=env)
    with binary.open('ab') as stream:
        stream.write(b'\0')
    try:
        env.action(source=[], target=[binary], env=env)
    except subprocess.CalledProcessError as failure:
        assert failure.returncode != 0
    else:
        raise AssertionError('oversized complete image accepted')
print('PASS actual post-action accepts exact fit, rejects one-byte overflow; NVS/PHY offsets and 4MiB bounds preserved')
