"""Packaging-tool regression: self IDs are not loads; missing real loads fail."""
from pathlib import Path
import importlib.util,tempfile,subprocess,plistlib
root=Path.cwd();spec=importlib.util.spec_from_file_location('package_tool',root/'scripts/package-macos-acceptance.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
with tempfile.TemporaryDirectory(prefix='dunlin-closure-',dir=root/'build/v51') as tmp:
 work=Path(tmp);app=work/'Owned.app';libdir=app/'Contents/Resources';exe=app/'Contents/MacOS/Owned';exe.parent.mkdir(parents=True);libdir.mkdir();(app/'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable':'Owned'}))
 a=work/'a.c';a.write_text('int owned(void){return 7;}');dep=libdir/'libowned.dylib';subprocess.run(['cc','-dynamiclib',str(a),'-Wl,-install_name,@rpath/libowned.dylib','-o',str(dep)],check=True)
 b=work/'main.c';b.write_text('extern int owned(void);int main(void){return owned()!=7;}');subprocess.run(['cc',str(b),str(dep),'-Wl,-rpath,@executable_path/../Resources','-o',str(exe)],check=True)
 assert module.closure(app)['missing']==[]
 # This dylib's self ID has no executable rpath match but is still valid.
 standalone=libdir/'unmatched-name.dylib';subprocess.run(['cc','-dynamiclib',str(a),'-Wl,-install_name,@rpath/identity-only.dylib','-o',str(standalone)],check=True);assert module.closure(app)['missing']==[]
 dep.unlink();missing=module.closure(app)['missing'];assert any(d['dependency']=='@rpath/libowned.dylib' for d in missing),missing
print('PASS packaging LC_ID_DYLIB excluded, valid real load resolves, removing loaded dylib fails closure. Tool test only.')
