#!/usr/bin/env python3
"""Pinned private WebSocket-capable libcurl; no system/Homebrew modifications."""
from pathlib import Path, PurePosixPath
import hashlib,json,os,platform,subprocess,sys,tarfile,urllib.request
root=Path(__file__).resolve().parents[1]
assert platform.system()=='Darwin'
pin_path=root/'vendor/media-curl/pin.json';pin=json.loads(pin_path.read_text());base=root/'build/native-deps'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
deps=Path(subprocess.check_output([sys.executable,'-B',str(root/'scripts/build-macos-deps.py')],cwd=root,text=True).strip())
key={'pin':sha(pin_path),'builder':sha(Path(__file__)),'native_dependencies':sha(deps/'manifest.json'),'arch':platform.machine()}
fingerprint=hashlib.sha256(json.dumps(key,sort_keys=True).encode()).hexdigest()[:16]
prefix=base/('media-curl-'+fingerprint);stamp=prefix/'manifest.json'
if stamp.exists():
 saved=json.loads(stamp.read_text())
 if saved['key']==key and all((prefix/a['path']).is_file() and sha(prefix/a['path'])==a['sha256'] for a in saved['artifacts']):print(prefix);raise SystemExit(0)
 raise SystemExit('Media transport cache changed; rebuild in a fresh owned prefix')
prefix.mkdir(parents=True,exist_ok=True)
archive=base/'downloads'/Path(pin['url']).name;archive.parent.mkdir(parents=True,exist_ok=True)
if not archive.exists():
 with urllib.request.urlopen(pin['url'],timeout=30) as response:
  assert response.url.startswith('https://curl.se/')
  data=response.read(20000001);assert len(data)<=20000000
  assert hashlib.sha256(data).hexdigest()==pin['sha256']
 archive.write_bytes(data)
assert sha(archive)==pin['sha256']
unpack=base/'sources'/('media-curl-'+fingerprint);unpack.mkdir(parents=True,exist_ok=True)
with tarfile.open(archive) as tar:
 members=tar.getmembers()
 for member in members:
  p=PurePosixPath(member.name)
  assert not p.is_absolute() and '..' not in p.parts and (member.isfile() or member.isdir())
 tar.extractall(unpack)
source=unpack/('curl-'+pin['version']);build=base/'objects'/('media-curl-'+fingerprint);build.mkdir(parents=True,exist_ok=True)
flags=['-DCMAKE_BUILD_TYPE=Release','-DCMAKE_OSX_DEPLOYMENT_TARGET='+pin['deployment'],'-DCMAKE_INSTALL_PREFIX='+str(prefix),'-DBUILD_SHARED_LIBS=OFF','-DBUILD_STATIC_LIBS=ON','-DBUILD_CURL_EXE=ON','-DBUILD_TESTING=OFF','-DCURL_USE_OPENSSL=ON','-DOPENSSL_ROOT_DIR='+str(deps),'-DOPENSSL_USE_STATIC_LIBS=ON','-DUSE_APPLE_SECTRUST=ON','-DCURL_DISABLE_WEBSOCKETS=OFF','-DUSE_LIBIDN2=OFF','-DCURL_USE_LIBPSL=OFF','-DUSE_NGHTTP2=OFF','-DCURL_BROTLI=OFF','-DCURL_ZSTD=OFF','-DCURL_USE_LIBSSH2=OFF','-DCURL_USE_GSSAPI=OFF','-DCURL_DISABLE_LDAP=ON','-DCURL_DISABLE_LDAPS=ON']
commands=[['cmake','-S',str(source),'-B',str(build),*flags],['cmake','--build',str(build),'--parallel','4'],['cmake','--install',str(build)]]
with (build/'build.log').open('a') as log:
 for command in commands:subprocess.run(command,cwd=root,stdout=log,stderr=subprocess.STDOUT,check=True)
version=subprocess.check_output([str(prefix/'bin/curl'),'--version'],text=True)
assert ' wss' in version and 'AppleSecTrust' in version
(prefix/'COPYING').write_bytes((source/'COPYING').read_bytes())
artifacts=[prefix/'lib/libcurl.a',prefix/'COPYING',*prefix.joinpath('include').rglob('*.h')]
stamp.write_text(json.dumps({'key':key,'commands':commands,'version':version,'artifacts':[{'path':str(p.relative_to(prefix)),'sha256':sha(p)} for p in artifacts],'limits':'Private static macOS 26-targeted build. Upstream full suite not run; actual integration and TLS tests are separate.'},indent=2)+'\n')
print(prefix)
