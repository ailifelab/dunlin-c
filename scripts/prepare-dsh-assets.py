#!/usr/bin/env python3
"""Build-only packaging of pinned optional hosts; never launches a plugin/engine."""
from pathlib import Path
import shutil,json,hashlib,sys,subprocess
root=Path(__file__).resolve().parents[1]
dest=root/(sys.argv[1] if len(sys.argv)>1 else 'build/v51/dsh-assets')
assert dest.is_relative_to(root/'build')
compiler=root/'build/v51/dsh-toolchain-v2'
assert (compiler/'package-lock.json').read_bytes()==(root/'hosts/node-dsh/toolchain-lock.json').read_bytes(),'Rebuild the pinned optional host toolchain'
assert json.loads((compiler/'node_modules/esbuild/package.json').read_text())['version']=='0.28.2','Run npm ci --prefix build/v51/dsh-toolchain --ignore-scripts with the pinned lockfile'
# This is a build-time dependency, independent of the opt-in runtime engine.
web=root/'build/v51/dsh-web-toolchain-v2';web.mkdir(parents=True,exist_ok=True)
lock=root/'compat/dsh/web/package-lock.json'
if not (web/'package-lock.json').exists() or (web/'package-lock.json').read_bytes()!=lock.read_bytes() or not (web/'node_modules/react/package.json').exists():
 shutil.copyfile(root/'compat/dsh/web/package.json',web/'package.json');shutil.copyfile(lock,web/'package-lock.json')
 subprocess.run(['npm','ci','--prefix',str(web),'--ignore-scripts','--no-audit','--no-fund','--userconfig','/dev/null','--cache',str(root/'build/v51/dsh-dependency-cache')],check=True)
subprocess.run(['node',str(root/'scripts/build-dsh-web.mjs'),str(web)],cwd=root,check=True)
dest.mkdir(parents=True,exist_ok=True)
(dest/'web').mkdir(parents=True,exist_ok=True)
shutil.copyfile(root/'build/v51/dsh-web-runtime/runtime.js',dest/'web/runtime.js')
shutil.copyfile(root/'build/v51/dsh-web-runtime/runtime.css',dest/'web/runtime.css')
shutil.copyfile(root/'compat/dsh/web/pin.json',dest/'web/pin.json')
# Match the immutable image workbench's embedded tldraw SDK exactly.
canvas_pin=json.loads((root/'compat/dsh/web/static-assets.json').read_text());canvas_notice=root/'compat/dsh/web'/canvas_pin['notice']
assert hashlib.sha256(canvas_notice.read_bytes()).hexdigest()==canvas_pin['notice_sha256']
assert json.loads(lock.read_text())['packages']['node_modules/@tldraw/assets']['integrity']==canvas_pin['integrity']
canvas_assets=web/'node_modules/@tldraw/assets'
assert json.loads((canvas_assets/'package.json').read_text())['version']=='5.3.0'
canvas_dest=dest/'web/tldraw/5.3.0';canvas_dest.mkdir(parents=True,exist_ok=True)
for directory in ['fonts','icons','translations','embed-icons','watermarks']:
 if (canvas_assets/directory).is_dir():shutil.copytree(canvas_assets/directory,canvas_dest/directory,dirs_exist_ok=True)

for name in ['react','react-dom','use-sync-external-store']:
 shutil.copyfile(web/'node_modules'/name/'LICENSE',dest/'web'/(name+'-LICENSE.txt'))
shutil.copyfile(root/'compat/dsh/web/vendor/ui-slots/LICENSE',dest/'web/DSH-LICENSE.txt')
# Retain notices for every locked browser dependency, including embedded fonts.
notices=dest/'web/licenses';notices.mkdir(exist_ok=True)
packages=json.loads(lock.read_text())['packages'];licenses=[]
shutil.copyfile(canvas_notice,notices/'tldraw-5.3.0-LICENSE.md')
shutil.copyfile(root/'compat/dsh/web/static-assets.json',notices/'tldraw-assets-provenance.json')
for relative,metadata in packages.items():
 if not relative:continue
 directory=web/relative
 if not directory.is_dir():continue
 name=relative.replace('node_modules/','').replace('/','__')
 files=[]
 for item in directory.iterdir():
  if item.is_file() and item.name.lower().startswith(('license','licence','copying','notice')):
   output=notices/(name+'__'+item.name);shutil.copyfile(item,output);files.append(output.name)
 licenses.append({'package':relative,'version':metadata.get('version'),'license':metadata.get('license'),'files':files})
(notices/'index.json').write_text(json.dumps(licenses,indent=2)+'\n')

shutil.copytree(root/'compat/dsh/vendor',dest/'compat/dsh/vendor',dirs_exist_ok=True)
shutil.copyfile(root/'compat/dsh/pin.json',dest/'compat/dsh/pin.json')
(dest/'hosts/node-dsh').mkdir(parents=True,exist_ok=True)
for source in (root/'hosts/node-dsh').glob('*.mjs'):shutil.copyfile(source,dest/'hosts/node-dsh'/source.name)
(dest/'hosts/python').mkdir(parents=True,exist_ok=True)
shutil.copyfile(root/'hosts/python/worker.py',dest/'hosts/python/worker.py')
shutil.copytree(compiler/'node_modules',dest/'toolchain/node_modules',dirs_exist_ok=True,ignore=shutil.ignore_patterns('.bin'))
shutil.copyfile(compiler/'package.json',dest/'toolchain/package.json')
shutil.copyfile(compiler/'package-lock.json',dest/'toolchain/package-lock.json')
files=[{'path':str(p.relative_to(dest)),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted(dest.rglob('*')) if p.is_file() and p.name!='assets.json']
(dest/'assets.json').write_text(json.dumps({'protocol':'dunlin.dsh.assets','commit':'1f81c047fc48fa190a683e469daa699857e9afe8','compiler':'esbuild@0.28.2','node':'26.13.0','files':files},indent=2)+'\n')
print(dest)
