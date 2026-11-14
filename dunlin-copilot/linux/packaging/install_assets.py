#!/usr/bin/env python3
"""Copy only entries in a prepared Linux DSH manifest, verifying native binaries and bytes."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import os
import shutil
import struct
import sys
import tempfile

source, destination = map(Path, sys.argv[1:3])
arch = sys.argv[3]
machine = {'arm64': 183, 'x64': 62}[arch]
manifest = source / 'assets.json'
record = json.loads(manifest.read_text())
if record.get('protocol') != 'dunlin.dsh.assets' or record.get('node') != '26.13.0':
    raise SystemExit('Unsupported DSH runtime manifest')
if source.resolve() == destination.resolve():
    raise SystemExit('The input and output asset locations must be separate')
if destination.is_symlink():
    raise SystemExit('Refusing a symbolic-link asset destination')
destination.mkdir(parents=True, exist_ok=True)
expected = {'assets.json'}
objects = 0
for item in record['files']:
    rel = PurePosixPath(item['path'])
    if rel.is_absolute() or '..' in rel.parts or str(rel) in expected or str(rel) == '.':
        raise SystemExit('Invalid asset path')
    origin = source / str(rel)
    if not origin.is_file() or origin.is_symlink() or source.resolve() not in origin.resolve().parents:
        raise SystemExit('Asset must be an ordinary file in the prepared directory: ' + str(rel))
    content = origin.read_bytes()
    if hashlib.sha256(content).hexdigest() != item['sha256']:
        raise SystemExit('Prepared asset checksum mismatch: ' + str(rel))
    if content[:4] == b'\x7fELF':
        if len(content) < 20 or struct.unpack_from('<H', content, 18)[0] != machine:
            raise SystemExit('Foreign ELF binary: ' + str(rel))
        objects += 1
    if content[:4] in (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe') or content[:2] == b'MZ':
        raise SystemExit('Non-Linux executable found: ' + str(rel))
    expected.add(str(rel))
    target = destination / str(rel)
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_symlink() or destination.resolve() not in target.resolve().parents:
        raise SystemExit('Output path escapes asset directory: ' + str(rel))
    # Replacing a file also prevents modifying an unrelated hard-linked file.
    with tempfile.NamedTemporaryFile(dir=str(target.parent), delete=False) as tmp:
        tmp.write(content)
        staged = Path(tmp.name)
    try:
        staged.chmod(0o755 if origin.stat().st_mode & 0o111 else 0o644)
        os.replace(str(staged), str(target))
    finally:
        if staged.exists():
            staged.unlink()
esbuild = 'toolchain/node_modules/@esbuild/linux-' + arch + '/bin/esbuild'
if esbuild not in expected or not objects:
    raise SystemExit('Target esbuild missing')
extra = [p for p in destination.rglob('*') if p.is_file() and str(p.relative_to(destination)) not in expected]
if extra:
    raise SystemExit('Stale files in output asset directory: ' + str(extra[0]))
output_manifest = destination / 'assets.json'
if output_manifest.is_symlink() or (output_manifest.exists() and output_manifest.stat().st_nlink != 1):
    raise SystemExit('Invalid destination manifest')
shutil.copyfile(str(manifest), str(output_manifest))
print('Validated', len(expected), 'files,', objects, 'Linux native binaries for', arch)
