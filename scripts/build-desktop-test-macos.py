#!/usr/bin/env python3
"""Owned macOS C service test build. No app/model/user-state launch or reset."""
from pathlib import Path
import subprocess,sys,hashlib,json,platform,datetime,argparse
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('output',nargs='?',default='build/v51/coretest');parser.add_argument('--sanitize',action='store_true');parser.add_argument('--deps-prefix',type=Path);options=parser.parse_args()
out=(root/options.output).resolve()
assert out.is_relative_to(root/'build') and platform.system()=='Darwin'
out.mkdir(parents=True,exist_ok=True)
sanitize=options.sanitize
flags=['-O1','-g','-fno-omit-frame-pointer','-fsanitize=address,undefined'] if sanitize else []
dependencies=[];dependency_flags=[];json_prefix=Path('/opt/homebrew/opt/json-c');crypto_prefix=Path('/opt/homebrew/opt/openssl@3')
if options.deps_prefix:
 dependency_prefix=options.deps_prefix.resolve();assert dependency_prefix.is_relative_to(root/'build/native-deps')
 stamp=dependency_prefix/'manifest.json';saved=json.loads(stamp.read_text());assert saved['pins']['deployment']=='26.0' and saved['key']['arch']==platform.machine()
 for item in saved['artifacts']:
  path=dependency_prefix/item['path'];assert path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest()==item['sha256'],path
 dependencies=[stamp,root/'vendor/native-macos/pins.json',root/'scripts/build-macos-deps.py',dependency_prefix/'lib/libjson-c.a',dependency_prefix/'lib/libcrypto.a',*dependency_prefix.joinpath('include').rglob('*.h')]
 json_prefix=crypto_prefix=dependency_prefix
 flags=[*(flags or ['-O2','-g']),'-mmacosx-version-min=26.0'];dependency_flags=['JSON_C_PREFIX='+str(dependency_prefix)]
subprocess.run(['make',f'BUILD={out.relative_to(root)}',*dependency_flags,str(out.relative_to(root)/'libdunlin.a'),str(out.relative_to(root)/'libdunlin-kernel.a'),*(['CFLAGS=-std=c11 -Wall -Wextra '+ ' '.join(flags)] if flags else [])],cwd=root,check=True)
media_prefix=Path(subprocess.check_output([sys.executable,'-B',str(root/'scripts/build-media-curl.py')],cwd=root,text=True).strip())
crypto_prefix=root/'build/native-deps/macos26'
dependencies += [media_prefix/'manifest.json',media_prefix/'lib/libcurl.a',crypto_prefix/'lib/libssl.a',crypto_prefix/'lib/libcrypto.a',root/'vendor/media-curl/pin.json',root/'scripts/build-media-curl.py',*media_prefix.joinpath('include').rglob('*.h')]
sources=sorted([*root.joinpath('desktop').glob('*.c'),*root.joinpath('modules').rglob('*.c'),*root.joinpath('compat/dsh').glob('*.c'),*root.joinpath('vendor/libyaml/src').glob('*.c')])
lib=out/'libdunlin-test.dylib';command=['cc','-dynamiclib',*flags,'-std=c11','-D_DARWIN_C_SOURCE','-DDUNLIN_WITH_DSH','-Wall','-Wextra','-Iinclude','-Ivendor/libyaml/include','-include','modules/config_yaml/build_config.h','-I'+str(json_prefix/'include'),'-I'+str(crypto_prefix/'include'),'-I'+str(media_prefix/'include'),*[str(p.relative_to(root)) for p in sources],str(out/'libdunlin.a'),str(out/'libdunlin-kernel.a'),str(json_prefix/'lib/libjson-c.a'),str(media_prefix/'lib/libcurl.a'),str(crypto_prefix/'lib/libssl.a'),str(crypto_prefix/'lib/libcrypto.a'),'-framework','SystemConfiguration','-framework','Security','-framework','CoreFoundation','-framework','CoreServices','-lsqlite3','-lz','-o',str(lib)]
subprocess.run(command,cwd=root,check=True)
inputs=[*sources,*root.joinpath('modules').rglob('*.h'),*root.joinpath('modules').rglob('*.inc'),*root.joinpath('vendor/libyaml').rglob('*.h'),root/'vendor/libyaml/pin.json',root/'vendor/libyaml/License',*root.joinpath('desktop').glob('*.h'),*root.joinpath('desktop').glob('*.inc'),*root.joinpath('compat').rglob('*.h'),*root.joinpath('include').rglob('*.h'),*root.joinpath('dunlin-compact/runtime').glob('*.c'),*root.joinpath('dunlin-compact/plugins').glob('*.c'),*root.joinpath('dunlin-compact/platform').glob('*.c'),*root.joinpath('dunlin-compact/platform').glob('*.h'),*root.joinpath('dunlin-compact/kernel').glob('*.c'),root/'Makefile',Path(__file__),*dependencies]
identity={'at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'environment':platform.platform(),'command':command,'inputs':[{'path':str(p.relative_to(root)),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted(set(inputs))],'artifact':{'path':str(lib.relative_to(root)),'sha256':hashlib.sha256(lib.read_bytes()).hexdigest()}}
snapshot=out/'history';snapshot.mkdir(exist_ok=True)
artifact=snapshot/('libdunlin-'+identity['artifact']['sha256']+'.dylib')
if not artifact.exists():
 import shutil
 shutil.copy2(lib,artifact)
identity['frozen_artifact']={'path':str(artifact.relative_to(root)),'sha256':identity['artifact']['sha256']}
manifest=snapshot/(identity['artifact']['sha256']+'.json')
manifest.write_text(json.dumps(identity,indent=2)+'\n')
(out/'inputs.json').write_text(json.dumps(identity,indent=2)+'\n');print('Test library',lib);print('Input manifest',manifest)
