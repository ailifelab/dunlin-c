#!/usr/bin/env python3
"""Build the private Linux desktop dependencies from checksum-pinned release sources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[3]
SQLITE = {
    'name': 'sqlite', 'version': '3.54.1', 'url': 'https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip',
    'sha3_256': '92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30',
}


def fetch(package, cache, offline):
    url = package['url']
    path = cache / url.rsplit('/', 1)[-1]
    algorithm = 'sha3_256' if 'sha3_256' in package else 'sha256'
    if not path.exists():
        if offline:
            raise SystemExit('Offline dependency missing: ' + str(path))
        with tempfile.NamedTemporaryFile(dir=cache, delete=False) as tmp:
            name = Path(tmp.name)
        try:
            with urllib.request.urlopen(url, timeout=90) as response, name.open('wb') as out:
                shutil.copyfileobj(response, out)
            if hashlib.new(algorithm, name.read_bytes()).hexdigest() != package[algorithm]:
                raise SystemExit('Release checksum mismatch: ' + path.name)
            os.replace(str(name), str(path))
        finally:
            if name.exists():
                name.unlink()
    if hashlib.new(algorithm, path.read_bytes()).hexdigest() != package[algorithm]:
        raise SystemExit('Cached release checksum mismatch: ' + path.name)
    return path


def build(prefix, cache, jobs, offline):
    release = {p['name']: p for p in json.loads((ROOT / 'vendor/native-macos/pins.json').read_text())['dependencies']}
    json_pin, ssl_pin = release['json-c'], release['openssl']
    curl_pin = json.loads((ROOT / 'vendor/media-curl/pin.json').read_text())
    packages = [SQLITE, json_pin, curl_pin, ssl_pin]
    archives = [fetch(p, cache, offline) for p in packages]
    prefix.mkdir(parents=True, exist_ok=True)
    inc, lib = prefix / 'include', prefix / 'lib'
    inc.mkdir(exist_ok=True)
    lib.mkdir(exist_ok=True)
    notices = prefix / 'licenses'
    notices.mkdir(exist_ok=True)
    cc, ar = os.environ.get('CC', 'cc'), os.environ.get('AR', 'ar')

    def run(argv, cwd):
        subprocess.run([str(a) for a in argv], cwd=str(cwd), check=True)

    with tempfile.TemporaryDirectory(prefix='dunlin-linux-deps-') as tmp:
        work = Path(tmp)
        with zipfile.ZipFile(str(archives[0])) as sqlite:
            for filename in ['sqlite3.c', 'sqlite3.h', 'sqlite3ext.h']:
                target = work / filename if filename.endswith('.c') else inc / filename
                member = next(s for s in sqlite.namelist() if s.endswith('/' + filename))
                target.write_bytes(sqlite.read(member))
        (notices / 'SQLite.txt').write_text((work / 'sqlite3.c').read_text().split('*/',1)[0] + '*/\nhttps://sqlite.org/copyright.html\n')
        run([cc, '-O2', '-fPIC', '-I' + str(inc), '-DSQLITE_ENABLE_FTS5', '-DSQLITE_THREADSAFE=1',
             '-DSQLITE_OMIT_LOAD_EXTENSION', '-c', 'sqlite3.c', '-o', 'sqlite3.o'], work)
        run([ar, 'rcs', lib / 'libsqlite3.a', 'sqlite3.o'], work)
        # Only the versioned archives matching the pinned digests reach this step.
        for archive in archives[1:]:
            run(['tar', '--no-same-owner', '-xf', archive], work)
        shutil.copyfile(work / ('json-c-' + json_pin['version']) / 'COPYING', notices / 'json-c.txt')
        shutil.copyfile(work / ('curl-' + curl_pin['version']) / 'COPYING', notices / 'curl.txt')
        build_json = work / 'json-build'
        build_json.mkdir()
        run(['cmake', '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TESTING=OFF', '-DENABLE_THREADING=ON',
             '-DCMAKE_POSITION_INDEPENDENT_CODE=ON', '-DCMAKE_INSTALL_PREFIX=' + str(prefix),
             '-DCMAKE_INSTALL_LIBDIR=lib', work / ('json-c-' + json_pin['version'])], build_json)
        run(['make', '-j' + str(jobs), 'install'], build_json)
        openssl = work / ('openssl-' + ssl_pin['version'])
        shutil.copyfile(openssl / 'LICENSE.txt', notices / 'OpenSSL.txt')
        run(['perl', './Configure', 'no-shared', 'no-module', 'no-tests', '-fPIC',
             '--prefix=' + str(prefix), '--libdir=lib', '--openssldir=/etc/ssl'], openssl)
        run(['make', '-j' + str(jobs), 'build_libs'], openssl)
        run(['make', 'install_dev'], openssl)
        curl = work / ('curl-' + curl_pin['version'])
        paths = ['--prefix=' + str(prefix), '--libdir=' + str(lib), '--with-openssl=' + str(prefix),
                 '--with-ca-path=/etc/ssl/certs', '--with-ca-bundle=/etc/ssl/certs/ca-certificates.crt']
        switches = ['--disable-shared', '--enable-static', '--with-pic', '--enable-websockets',
                    '--without-libpsl', '--without-libidn2', '--without-brotli', '--without-zstd',
                    '--without-nghttp2', '--without-libssh2', '--disable-ldap', '--disable-ldaps',
                    '--disable-manual', '--disable-docs', '--disable-ftp', '--disable-file',
                    '--disable-dict', '--disable-gopher', '--disable-imap', '--disable-mqtt',
                    '--disable-pop3', '--disable-rtsp', '--disable-smb', '--disable-smtp',
                    '--disable-telnet', '--disable-tftp']
        run(['./configure'] + paths + switches, curl)
        run(['make', '-C', 'lib', '-j' + str(jobs), 'install'], curl)
        run(['make', '-C', 'include', 'install'], curl)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix', type=Path)
    parser.add_argument('--cache', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--offline', action='store_true')
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    build(args.prefix.resolve(), args.cache.resolve(), max(1, args.jobs), args.offline)
