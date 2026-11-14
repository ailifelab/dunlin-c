#!/usr/bin/env python3
"""Package only files installed by the client's Makefile."""
import argparse
import re
from pathlib import Path
import subprocess
import tempfile
import shutil

ROOT = Path(__file__).resolve().parents[3]
LINUX = ROOT / 'dunlin-copilot/linux'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
args = parser.parse_args()
# Both native desktop frontends use the version in the public Xcode project.
settings = (ROOT / 'dunlin-copilot/macos/Configuration/Base.xcconfig').read_text()
release_number = re.search(r'^MARKETING_VERSION\s*=\s*(\d+\.\d+\.\d+)\s*$', settings, re.MULTILINE)
if not release_number:
    raise SystemExit('Public desktop marketing version is missing')
version = release_number.group(1) + '~dev'
release = dict(line.rstrip().split('=', 1) for line in Path('/etc/os-release').read_text().splitlines() if '=' in line)
distribution, series = (release.get(k, '').strip('"') for k in ('ID', 'VERSION_ID'))
if distribution != 'ubuntu' or series not in ('18.04', '24.04'):
    raise SystemExit('Build the Ubuntu package on the matching supported release')
arch = subprocess.check_output(['dpkg', '--print-architecture'], universal_newlines=True).strip()
output = args.build.resolve()
with tempfile.TemporaryDirectory(dir=str(output), prefix='deb-stage-') as tmp:
    stage = Path(tmp)
    subprocess.run(['make', 'install', 'BUILD=' + str(output), 'DESTDIR=' + str(stage), 'PREFIX=/usr'],
                   cwd=str(LINUX), check=True)
    # Ask dpkg for shared-library dependencies of the two real ELF files. The
    # internal shared object is delivered by this same package.
    source = stage / 'debian'
    source.mkdir()
    (source / 'control').write_text('Source: dunlin-copilot\nSection: utils\nPriority: optional\n'
        'Maintainer: AI Life Lab <ailifelab@users.noreply.github.com>\nStandards-Version: 4.1.4\n\n'
        'Package: dunlin-copilot\nArchitecture: any\nDescription: Dunlin Copilot GTK desktop client\n')
    (source / 'shlibs.local').write_text('libdunlin-desktop 1 dunlin-copilot\n')
    lib = stage / 'usr/lib/dunlin-copilot'
    # Inspection/debug information belongs in local build outputs. Strip only
    # the staged copies, including source paths from private build directories.
    subprocess.run(['strip', '--strip-unneeded', str(stage / 'usr/bin/dunlin-copilot'),
                    str(lib / 'libdunlin-desktop.so.1')], check=True)
    deps = subprocess.check_output(['dpkg-shlibdeps', '-O', '-l' + str(lib), '-xdunlin-copilot',
          '-e' + str(stage / 'usr/bin/dunlin-copilot'), '-e' + str(lib / 'libdunlin-desktop.so.1')],
          cwd=str(stage), universal_newlines=True).strip().split('=', 1)[1]
    shutil.rmtree(str(source))
    meta = stage / 'DEBIAN'
    meta.mkdir()
    (meta / 'control').write_text('Package: dunlin-copilot\nVersion: ' + version + '\nArchitecture: ' + arch +
        '\nSection: utils\nPriority: optional\nMaintainer: AI Life Lab <ailifelab@users.noreply.github.com>\n' +
        'Depends: ' + deps + ', ca-certificates, libatomic1, libstdc++6\nDescription: Dunlin Copilot GTK desktop client\n')
    archive = output / ('dunlin-copilot_' + version + '_' + arch + '.deb')
    archive = archive.with_name(archive.stem + '_ubuntu' + series + '.deb')
    subprocess.run(['dpkg-deb', '--root-owner-group', '--build', str(stage), str(archive)], check=True)
    print(str(archive))
