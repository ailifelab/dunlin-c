#!/usr/bin/env python3
"""C-owned optional environment and real JS/Python extensions with actual command effects."""
import ctypes as C,json,sys,pathlib,tempfile,sqlite3,hashlib
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_extensions_call',[P,S,P],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
with tempfile.TemporaryDirectory(prefix='dunlin-extensions-') as tmp:
 root=pathlib.Path(tmp);private=root/'private';private.mkdir(mode=0o700);assets=pathlib.Path(sys.argv[2]).resolve();db=lib.dc_open(str(private).encode());assert db
 def req(op,**kw):
  raw=lib.dc_extensions_call(db,json.dumps(dict(op=op,**kw)).encode(),None);assert raw
  try:return json.loads(C.string_at(raw))
  finally:lib.dc_free(raw)
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r.get('data')
 def bad(op,**kw):
  r=req(op,**kw);assert not r['ok'],r;return r
 assert ok('compat.status')['state']=='off';assert not (private/'Compatibility').exists()
 ok('extensions.assets',path=str(assets))

 package=root/'theme';package.mkdir()
 manifest={'protocol':'dunlin.extension','major':1,'packageId':'theme-projection','version':'1.0.0','displayName':'Owned theme','kind':'theme','engines':{'dunlin':'desktop@1','host':'native-theme'},'capabilities':[],'requires':[],'dependencies':[],'theme':{'scheme':'dark','tokens':{'accent':'#39DDAA','background':'#112233','surface':'#182D3D','text':'#F2F7FA','secondary':'#AFC7D1','untrusted':'url(https://invalid.example)'}}}
 (package/'dunlin.json').write_text(json.dumps(manifest));ok('extensions.import',path=str(package))
 theme=ok('extensions.themes')[0];assert theme.get('tokens')=={k:v for k,v in manifest['theme']['tokens'].items() if k not in ['accent','untrusted']},theme
 assert theme['accent']=='#39DDAA'
 badtheme=root/'badtheme';badtheme.mkdir();manifest['packageId']='bad-theme';manifest['theme']['tokens']['accent']='#123456\x00ignored'
 (badtheme/'dunlin.json').write_text(json.dumps(manifest));assert bad('extensions.import',path=str(badtheme))['error']=='unsafe_extension_directory'
 assert len(ok('extensions.themes'))==1,'embedded NUL color admitted'
 lib.dc_close(db)
 print('PASS data-only theme background/surface/text/secondary projection; unknown token and embedded-NUL accent excluded')
