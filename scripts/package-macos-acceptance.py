#!/usr/bin/env python3
"""Copy a complete built app into an isolated, locally signed acceptance app.

Never launches an app, engine or model. A successful package/closure check is not
native launch or product acceptance; verify the resulting window separately.
"""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
MAGIC={b'\xcf\xfa\xed\xfe',b'\xce\xfa\xed\xfe',b'\xfe\xed\xfa\xcf',b'\xfe\xed\xfa\xce',b'\xca\xfe\xba\xbe',b'\xbe\xba\xfe\xca',b'\xca\xfe\xba\xbf',b'\xbf\xba\xfe\xca'}

def owned(path,areas):
    path=path.resolve()
    if not any(path!=ROOT/a and path.is_relative_to(ROOT/a) for a in areas):
        raise ValueError('path must stay inside '+','.join(areas))
    return path

def info(app):
    data=plistlib.loads((app/'Contents/Info.plist').read_bytes())
    name=data.get('CFBundleExecutable','')
    if not isinstance(name,str) or not name or Path(name).name!=name:
        raise ValueError('invalid bundle executable')
    exe=app/'Contents/MacOS'/name
    if not exe.is_file():raise ValueError('bundle executable missing')
    return data,exe

def command(args):
    return subprocess.check_output(args,text=True,stderr=subprocess.STDOUT)

def image(path):
    with path.open('rb') as f:return f.read(4) in MAGIC

def rpaths(path):
    lines=command(['otool','-l',str(path)]).splitlines();out=[]
    for i,line in enumerate(lines):
        if line.strip()=='cmd LC_RPATH':
            for following in lines[i+1:i+5]:
                if following.strip().startswith('path '):
                    out.append(following.strip()[5:].rsplit(' (offset ',1)[0]);break
    return out

def expand(path,loader,exe):
    for prefix,base in [('@loader_path',loader.parent),('@executable_path',exe.parent)]:
        if path==prefix:return base
        if path.startswith(prefix+'/'):return base/path[len(prefix)+1:]
    return Path(path) if path.startswith('/') else None

def closure(app):
    _,exe=info(app);images=[];missing=[]
    executable_paths=rpaths(exe)
    for path in sorted(app.rglob('*')):
        if not path.is_file():continue
        if not path.resolve().is_relative_to(app):raise ValueError('bundle file escapes app')
        if not image(path):continue
        dependencies=[]
        # otool -L also prints LC_ID_DYLIB. That is the image's install name,
        # not a dependency that must resolve through this executable's rpaths.
        own_ids={line.strip() for line in command(['otool','-D',str(path)]).splitlines()[1:] if line.strip()}
        search=[expand(p,path,exe) for p in rpaths(path)]+[expand(p,exe,exe) for p in executable_paths]
        for line in command(['otool','-L',str(path)]).splitlines()[1:]:
            if ' (compatibility version ' not in line:continue
            dep=line.strip().split(' (compatibility version ',1)[0]
            if dep in own_ids:continue
            if dep.startswith(('/System/Library/','/usr/lib/')):
                dependencies.append(dict(name=dep,kind='system'));continue
            candidates=[base/dep[7:] for base in search if base is not None] if dep.startswith('@rpath/') else [expand(dep,path,exe)]
            found=next((p.resolve() for p in candidates if p is not None and p.is_file() and p.resolve().is_relative_to(app)),None)
            dependencies.append(dict(name=dep,resolved=str(found.relative_to(app)) if found else None))
            if found is None:missing.append(dict(image=str(path.relative_to(app)),dependency=dep))
        images.append(dict(path=str(path.relative_to(app)),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),dependencies=dependencies))
    if not images:raise ValueError('bundle has no Mach-O executable')
    return dict(images=images,missing=missing)

def running(app):
    prefix=str(app)+'/Contents/MacOS/'
    return [line.strip() for line in command(['ps','-axo','pid=,comm=']).splitlines() if len(line.split(None,1))==2 and line.split(None,1)[1].startswith(prefix)]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--data-dir',type=Path)
    parser.add_argument('--check-only',action='store_true')
    parser.add_argument('--report',type=Path,required=True)
    args=parser.parse_args()
    if sys.platform!='darwin':parser.error('macOS packaging only')
    source=owned(args.source,['build','dist']);report=owned(args.report,['build','reports'])
    if report.exists():parser.error('report already exists; retain prior evidence')
    result=dict(at=datetime.datetime.now(datetime.timezone.utc).isoformat(),source=str(source.relative_to(ROOT)),script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),limits='Local dependency closure/signature only. No launch, model/tool approval, developer signing, distribution or full product claim.')
    try:
        source_info,_=info(source);result['source_closure']=closure(source)
        if result['source_closure']['missing']:raise ValueError('missing non-system dependency')
        if not args.check_only:
            if args.output is None or args.data_dir is None:raise ValueError('output/data-dir required')
            output=owned(args.output,['build']);data_dir=owned(args.data_dir,['build'])
            if output==source or output.is_relative_to(source) or source.is_relative_to(output):raise ValueError('source and output must be separate')
            if output.suffix!='.app' or data_dir.is_relative_to(output) or output.is_relative_to(data_dir):raise ValueError('app/data roots must be separate')
            if running(output):raise ValueError('refusing to replace a running acceptance app')
            if output.exists() and info(output)[0].get('CFBundleIdentifier')!='org.dunlin.desktop.acceptance':raise ValueError('refusing to replace a non-acceptance app')
            tag=datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
            staged=output.with_name(output.stem+'.next-'+tag+'.app');backup=output.with_name(output.stem+'.before-'+tag+'.app')
            if staged.exists() or backup.exists():raise ValueError('staging/backup collision')
            output.parent.mkdir(parents=True,exist_ok=True)
            shutil.copytree(source,staged,symlinks=True)
            source_info.update(CFBundleIdentifier='org.dunlin.desktop.acceptance',CFBundleName='Dunlin Copilot 验收',CFBundleDisplayName='Dunlin Copilot 验收',DunlinAcceptanceDataDirectory=str(data_dir))
            # Preserve CFBundleExecutable and every sibling Debug dylib; Xcode
            # may emit a tiny entry stub plus a separate implementation image.
            (staged/'Contents/Info.plist').write_bytes(plistlib.dumps(source_info))
            result['commands']=[]
            for cmd in [['codesign','--force','--deep','--sign','-',str(staged)],['codesign','--verify','--deep','--strict',str(staged)]]:
                result['commands'].append(dict(command=cmd,output=command(cmd),exit_code=0))
            result['output_closure']=closure(staged)
            if result['output_closure']['missing']:raise ValueError('staged dependency missing')
            if running(output):raise ValueError('acceptance app started during packaging; staged app retained')
            if output.exists():os.replace(output,backup);result['previous_bundle']=str(backup.relative_to(ROOT))
            os.replace(staged,output);result['output']=str(output.relative_to(ROOT));result['data_dir']=str(data_dir.relative_to(ROOT))
        result['status']='pass';code=0
    except (ValueError,OSError,subprocess.CalledProcessError) as e:
        result['status']='fail';result['error']=str(e);code=1
        if isinstance(e,subprocess.CalledProcessError):result['command_output']=e.output
    report.parent.mkdir(parents=True,exist_ok=True);report.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    print(result['status'],str(report.relative_to(ROOT)))
    return code

if __name__=='__main__':raise SystemExit(main())
