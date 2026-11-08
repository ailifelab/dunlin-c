"""Check the complete ESP image, including headers/hash, against its app partition.
PlatformIO's ELF section size can fit while the emitted flash image overflows.
"""
Import('env')
from pathlib import Path
import subprocess


def check_image(source, target, env):
    sdk = Path(env.PioPlatform().get_package_dir('framework-espidf'))
    build = Path(env.subst('$BUILD_DIR'))
    subprocess.run([
        env.subst('$PYTHONEXE'), str(sdk / 'components/partition_table/check_sizes.py'),
        '--offset', '0x8000', 'partition', '--type', 'app',
        str(build / 'partitions.bin'), str(target[0]),
    ], check=True)


env.AddPostAction('$BUILD_DIR/${PROGNAME}.bin', check_image)
