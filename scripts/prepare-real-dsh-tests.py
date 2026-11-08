#!/usr/bin/env python3
"""Fetch exact npm artifacts for an owned compatibility audit; never runs scripts.
Usage: python3 -B scripts/prepare-real-dsh-tests.py
No user config, credentials, or plugin execution. Output stays under build/.
"""
from pathlib import Path,PurePosixPath
import urllib.request,urllib.parse,json,hashlib,base64,tarfile,io
root=Path(__file__).resolve().parents[1];base=root/'build/v51/real-dsh-published';base.mkdir(parents=True,exist_ok=True)
report=root/'reports/v51-current/real-dsh-published-inputs.json';report.parent.mkdir(parents=True,exist_ok=True)
pins=json.loads((root/'catalogs/dsh-test-pins.json').read_text());results=[]
for pin in pins:
 name=pin['name'];version=pin['version'];url='https://registry.npmjs.org/'+urllib.parse.quote(name,safe='')+'/'+urllib.parse.quote(version,safe='')
 with urllib.request.urlopen(url,timeout=30) as response:meta=json.load(response)
 dist=meta['dist'];tarball=dist['tarball'];assert tarball.startswith('https://registry.npmjs.org/');integrity=dist['integrity'];algorithm,value=integrity.split('-',1);assert algorithm=='sha512'
 with urllib.request.urlopen(tarball,timeout=30) as response:data=response.read(33554433)
 assert len(data)<=33554432 and base64.b64encode(hashlib.sha512(data).digest()).decode()==value
 assert integrity==pin['integrity'] and hashlib.sha256(data).hexdigest()==pin['archiveSHA256'],'Fixed release bytes changed'
 dest=base/(name.replace('/','_')+'-'+version);dest.mkdir(exist_ok=True);archive=tarfile.open(fileobj=io.BytesIO(data));total=0
 for item in archive:
  path=PurePosixPath(item.name);assert path.parts[0]=='package' and '..' not in path.parts
  if item.isdir():continue
  assert item.isfile() and item.size<=16777216;total+=item.size;assert total<=67108864
  target=dest/Path(*path.parts[1:]);assert not target.exists(),'Use a fresh owned output path';target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(archive.extractfile(item).read())
 package=json.loads((dest/'package.json').read_text());assert package['name']==name and package['version']==version;entry=package.get('main','index.js').removeprefix('./');assert (dest/entry).is_file()
 manifest={'protocol':'dunlin.extension','major':1,'packageId':name,'version':version,'displayName':name,'kind':'plugin','engines':{'dunlin':'desktop@1','host':'node-dsh','dsh':'0.2.1-rc.3'},'entry':entry,'capabilities':['trusted-full-host'],'requires':[],'dependencies':[]}
 (dest/'dunlin.json').write_text(json.dumps(manifest)+'\n');results.append(dict(pin,path=str(dest.relative_to(root)),tarball=tarball,integrity=integrity,archiveSHA256=hashlib.sha256(data).hexdigest(),entry=entry))
report.write_text(json.dumps(results,indent=2)+'\n');print('Fetched and verified',len(results),'exact npm packages; no scripts executed')
