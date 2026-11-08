#!/usr/bin/env python3
"""Pinned local static dependencies; never changes Homebrew/system installations.
All downloads, source, builds, logs and install files stay in owned build paths.
Stdout is only the prefix for the native build caller.
"""
from pathlib import Path, PurePosixPath
import datetime, hashlib, json, os, platform, posixpath, subprocess, sys, tarfile, urllib.request

root=Path(__file__).resolve().parents[1]
pins_path=root/'vendor/native-macos/pins.json'
pins=json.loads(pins_path.read_text())
assert platform.system()=='Darwin' and platform.machine() in ('arm64','x86_64')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
base=root/'build/native-deps';prefix=base/'macos26'
key=dict(pins_sha256=sha(pins_path),builder_sha256=sha(Path(__file__)),arch=platform.machine(),
        sdk=subprocess.check_output(['xcrun','--show-sdk-version'],text=True).strip(),
        compiler=subprocess.check_output(['xcrun','clang','--version'],text=True).splitlines()[0])
stamp=prefix/'manifest.json'
if stamp.is_file():
    saved=json.loads(stamp.read_text())
    if saved['key']==key and all((prefix/f['path']).is_file() and sha(prefix/f['path'])==f['sha256'] for f in saved['artifacts']):
        print(prefix);raise SystemExit(0)
    raise SystemExit('Native dependency cache identity changed. Preserve it and select a fresh owned build prefix before rebuilding.')
marker=prefix/'owned.json'
if prefix.exists() and not marker.is_file():raise SystemExit('Refusing an unowned native dependency prefix')
prefix.mkdir(parents=True,exist_ok=True)
if marker.exists():
    if json.loads(marker.read_text())!=key:raise SystemExit('Incomplete dependency prefix belongs to different build inputs')
else:marker.write_text(json.dumps(key,indent=2)+'\n')
env=os.environ.copy();env['MACOSX_DEPLOYMENT_TARGET']=pins['deployment']
commands=[]
def run(args,cwd,log):
    commands.append(dict(command=list(map(str,args)),cwd=str(cwd.relative_to(root))))
    subprocess.run(list(map(str,args)),cwd=cwd,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
def json_tests(build,source,log):
    if subprocess.check_output(['/bin/sh','-c','echo -n probe'])==b'probe':
        run(['ctest','--test-dir',build,'--output-on-failure'],root,log);return
    # Darwin's POSIX sh prints "-n" literally. Upstream's CLI fixture uses
    # echo -n, so run ALL unchanged upstream assertions under bash, preserving
    # CTest's environment/workdir/arguments. Never skip a failed assertion or
    # replace upstream expected output with newly generated traces.
    query=['ctest','--test-dir',str(build),'--show-only=json-v1']
    tests=json.loads(subprocess.check_output(query,cwd=root,env=env))
    (build/'upstream-tests-bash.json').write_text(json.dumps(tests,indent=2)+'\n')
    for test in tests['tests']:
        command=test['command'];assert len(command)==1 and command[0].endswith('.test') and Path(command[0]).is_relative_to(source)
        properties={p['name']:p['value'] for p in test.get('properties',[])}
        test_env=env.copy()
        for value in properties.get('ENVIRONMENT',[]):
            key,value=value.split('=',1);test_env[key]=value
        cwd=Path(properties.get('WORKING_DIRECTORY',str(build)))
        args=['/bin/bash',*command];commands.append(dict(command=args,cwd=str(cwd.relative_to(root)),environment=properties.get('ENVIRONMENT',[])))
        log.write('\nUnchanged upstream test via bash: '+test['name']+'\n');log.flush()
        subprocess.run(args,cwd=cwd,env=test_env,stdout=log,stderr=subprocess.STDOUT,check=True)
source_files=[]
for dependency in pins['dependencies']:
    archive=base/'downloads'/Path(dependency['url']).name;archive.parent.mkdir(parents=True,exist_ok=True)
    if not archive.exists():
        print('Downloading pinned '+dependency['name'],file=sys.stderr,flush=True)
        staging=archive.with_suffix(archive.suffix+'.partial')
        request=urllib.request.Request(dependency['url'],headers={'User-Agent':'Dunlin-C-native-build'})
        try:
            with urllib.request.urlopen(request,timeout=60) as response,staging.open('wb') as output:
                size=0
                if not response.url.startswith('https://'):raise RuntimeError('Non-HTTPS dependency redirect')
                while True:
                    block=response.read(262144)
                    if not block:break
                    size+=len(block)
                    if size>104857600:raise RuntimeError('Dependency archive exceeds download bound')
                    output.write(block)
            if sha(staging)!=dependency['sha256']:raise RuntimeError('Dependency archive hash mismatch')
            staging.replace(archive)
        finally:
            if staging.exists():staging.unlink()
    if sha(archive)!=dependency['sha256']:raise SystemExit('Cached dependency archive hash mismatch: '+str(archive))
    unpack=base/'sources'/dependency['sha256'];unpack.mkdir(parents=True,exist_ok=True)
    ready=unpack/'extracted.json'
    if not ready.exists():
        with tarfile.open(archive) as tar:
            members=tar.getmembers()
            entries={member.name:member for member in members}
            for member in members:
                p=PurePosixPath(member.name)
                if p.is_absolute() or '..' in p.parts or not(member.isdir() or member.isfile() or member.issym() or member.islnk()):raise RuntimeError('Unsupported dependency archive entry: '+member.name)
                for ancestor in p.parents:
                    if str(ancestor) in entries and not entries[str(ancestor)].isdir():raise RuntimeError('Non-directory archive ancestor')
            # Upstream test runners include file symlinks. Materialize only
            # bounded, archive-internal chains ending at regular files. Never
            # create symlinks that could redirect later extraction writes.
            def target(member,seen):
                if member.name in seen or len(seen)>=32:raise RuntimeError('Cyclic archive link')
                if member.isfile():return member
                if not(member.issym() or member.islnk()):raise RuntimeError('Archive link is not a file')
                name=posixpath.normpath(posixpath.join(posixpath.dirname(member.name),member.linkname) if member.issym() else member.linkname)
                if name.startswith('/') or '..' in PurePosixPath(name).parts or name not in entries:raise RuntimeError('Archive link escapes source')
                return target(entries[name],seen|{member.name})
            links=[(member,target(member,set())) for member in members if member.issym() or member.islnk()]
            tar.extractall(unpack,members=[member for member in members if member.isdir() or member.isfile()])
            for member,resolved in links:
                path=unpack/member.name;path.parent.mkdir(parents=True,exist_ok=True)
                with tar.extractfile(resolved) as source,path.open('wb') as output:
                    while True:
                        block=source.read(262144)
                        if not block:break
                        output.write(block)
                path.chmod(resolved.mode&0o777)
        ready.write_text(json.dumps(dict(archive_sha256=dependency['sha256']))+'\n')
    directories=[p for p in unpack.iterdir() if p.is_dir()];assert len(directories)==1
    source=directories[0];build=base/'objects'/(dependency['name']+'-26-'+platform.machine());build.mkdir(parents=True,exist_ok=True)
    log_path=build/'build.log'
    print('Building '+dependency['name']+' for macOS '+pins['deployment'],file=sys.stderr,flush=True)
    with log_path.open('a') as log:
        log.write('\nBuild '+datetime.datetime.now(datetime.timezone.utc).isoformat()+'\n');log.flush()
        if dependency['name']=='json-c':
            run(['cmake','-S',source,'-B',build,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_OSX_DEPLOYMENT_TARGET='+pins['deployment'],'-DCMAKE_INSTALL_PREFIX='+str(prefix),'-DBUILD_SHARED_LIBS=OFF','-DBUILD_TESTING=ON','-DENABLE_THREADING=ON'],root,log)
            run(['cmake','--build',build,'--parallel','4'],root,log)
            json_tests(build,source,log)
            run(['cmake','--install',build],root,log)
            license_source=source/'COPYING'
        else:
            target='darwin64-arm64-cc' if platform.machine()=='arm64' else 'darwin64-x86_64-cc'
            run(['perl',source/'Configure',target,'no-shared','no-module','no-tests','--prefix='+str(prefix),'--libdir=lib','-mmacosx-version-min='+pins['deployment']],build,log)
            run(['make','-j4','build_libs'],build,log)
            run(['make','install_dev'],build,log)
            license_source=source/'LICENSE.txt'
        license_destination=prefix/'licenses'/(dependency['name']+'.txt');license_destination.parent.mkdir(exist_ok=True);license_destination.write_bytes(license_source.read_bytes())
    source_files.append(dict(archive=str(archive.relative_to(root)),sha256=sha(archive),log=str(log_path.relative_to(root)),log_sha256=sha(log_path)))
artifacts=[p for folder in ['include','lib','licenses'] for p in (prefix/folder).rglob('*') if p.is_file()]
assert (prefix/'lib/libjson-c.a').is_file() and (prefix/'lib/libcrypto.a').is_file()
result=dict(at=datetime.datetime.now(datetime.timezone.utc).isoformat(),key=key,pins=pins,commands=commands,sources=source_files,
            artifacts=[dict(path=str(p.relative_to(prefix)),sha256=sha(p)) for p in sorted(artifacts)],
            limits='Owned local static builds with macOS 26 deployment target on SDK '+key['sdk']+'. Internal file links are materialized as identical regular-file content; pinned archive bytes are unchanged. Does not establish execution on a real macOS 26 system. All json-c upstream assertions run through CTest or unchanged scripts under bash when Darwin sh lacks echo -n. OpenSSL upstream full suite is not run; consumed crypto paths require application integration checks.')
stamp.write_text(json.dumps(result,indent=2)+'\n');print(prefix)
