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
 plugin=root/'node';plugin.mkdir();manifest={'protocol':'dunlin.extension','major':1,'packageId':'node-fixture','version':'1.0.0','displayName':'Node fixture','kind':'plugin','engines':{'dunlin':'desktop@1','host':'node-dsh','dsh':'0.2.1-rc.3'},'entry':'index.ts','capabilities':['trusted-full-host'],'requires':['dunlin.contributions@1'],'dependencies':[]}
 (plugin/'dunlin.json').write_text(json.dumps(manifest));(plugin/'index.ts').write_text("export const name='fixture';export const inject=['dunlin'];export function apply(ctx){ctx.dunlin.registerCommand(ctx,'hello',p=>({text:'hello '+p.name}));ctx.dunlin.registerCommand(ctx,'fail',()=>{throw Error('owned failure')});ctx.dunlin.registerCommand(ctx,'fail2',()=>{throw TypeError('second private failure')});ctx.dunlin.contribute(ctx,'command',{kind:'command',title:'Hello',command:'hello',id:'untrusted-id',plugin:'untrusted-owner'})}")
 assert bad('extensions.import',path=str(plugin))['error']=='dsh_environment_not_ready';assert not ok('extensions.list')
 status=ok('compat.enable',node='/opt/homebrew/bin/node');assert status['enabled'] and status['healthy'];generation=status['generation'];assert ok('compat.enable',node='/opt/homebrew/bin/node')['generation']==generation
 rows=ok('extensions.import',path=str(plugin));row=rows[0];assert not row['enabled'];ref=dict(source=row['source'],package=row['package'],version=row['version'])
 rows=ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True);assert rows[0]['active'];contributions=ok('extensions.contributions');assert contributions[0]['kind']=='command' and contributions[0]['plugin']==row['runtimeID'] and contributions[0]['id'].startswith(row['runtimeID']+':')
 r=ok('extensions.command',source=ref['source'],package=ref['package'],command='hello',arguments={'name':'Dunlin'});assert r['data']['text']=='hello Dunlin'
 bad('extensions.command',source=ref['source'],package=ref['package'],command='fail',arguments={});assert ok('extensions.list')[0]['versions'][0]['quarantined'];assert not ok('extensions.contributions');assert ok('extensions.faults')[0]['phase']=='command'
 diagnostic=ok('extensions.faults')[0].get('diagnostic','');assert 'index.ts:1:' in diagnostic and 'Error' in diagnostic,diagnostic
 assert 'owned failure' not in diagnostic
 first_diagnostic=diagnostic
 ok('extensions.recover',**ref);ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)
 audit=sqlite3.connect(private/'conversations.sqlite')
 audit.execute("CREATE TRIGGER deny_fault_log BEFORE INSERT ON extension_faults BEGIN SELECT RAISE(ABORT,'owned audit failure'); END");audit.commit()
 bad('extensions.command',source=ref['source'],package=ref['package'],command='fail2',arguments={})
 assert ok('extensions.faults')[0]['diagnostic']==first_diagnostic,'failed log insertion overwrote an older diagnostic'
 assert not ok('extensions.list')[0]['active']
 audit.execute('DROP TRIGGER deny_fault_log');audit.commit();audit.close()

 bad('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)
 ok('extensions.recover',**ref);assert not ok('extensions.list')[0]['versions'][0]['quarantined'];ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True);ok('extensions.bind',**ref,enabled=False)
 page=root/'page';page.mkdir();page_manifest=dict(manifest,packageId='page-fixture')
 (page/'dunlin.json').write_text(json.dumps(page_manifest))
 (page/'index.ts').write_text("export const inject=['dunlin'];export function apply(ctx){ctx.dunlin.registerCommand(ctx,'review',()=>({text:'reviewed'}));ctx.dunlin.contribute(ctx,'panel',{kind:'sidebar',title:'Project review',body:'Owned plugin page',actions:[{id:'review',title:'Review',command:'review'}],sections:[{id:'notes',title:'Notes',body:'No hidden execution'}]})}")
 page_row=next(x for x in ok('extensions.import',path=str(page)) if x['package']=='page-fixture');page_ref=dict(source=page_row['source'],package=page_row['package'],version=page_row['version'])
 ok('extensions.bind',**page_ref,enabled=True,acceptHostPermissions=True)
 panel=next(x for x in ok('extensions.contributions') if x['kind']=='sidebar');assert panel['sections'][0]['body']=='No hidden execution' and panel['actions'][0]['command']=='review'
 ok('extensions.bind',**page_ref,enabled=False);assert not any(x['kind']=='sidebar' for x in ok('extensions.contributions'))
 invalid=root/'invalid-page';invalid.mkdir();invalid_manifest=dict(manifest,packageId='invalid-page')
 (invalid/'dunlin.json').write_text(json.dumps(invalid_manifest));(invalid/'index.ts').write_text("export const inject=['dunlin'];export function apply(ctx){ctx.dunlin.contribute(ctx,'broken',{kind:'sidebar',actions:'not-an-array'})}")
 invalid_row=next(x for x in ok('extensions.import',path=str(invalid)) if x['package']=='invalid-page');invalid_ref=dict(source=invalid_row['source'],package=invalid_row['package'],version=invalid_row['version'])
 bad('extensions.bind',**invalid_ref,enabled=True,acceptHostPermissions=True)
 invalid_row=next(x for x in ok('extensions.list') if x['package']=='invalid-page');assert not invalid_row['active'] and invalid_row['versions'][0]['quarantined']
 assert not any(x['kind']=='sidebar' for x in ok('extensions.contributions'))
 theme_plugin=root/'dsh-theme';theme_plugin.mkdir()
 theme_manifest=dict(manifest,packageId='dsh-theme-fixture',requires=['dsh.theme.register@1'])
 (theme_plugin/'dunlin.json').write_text(json.dumps(theme_manifest))
 (theme_plugin/'index.ts').write_text("export const inject=['theme'];export function apply(ctx){ctx.effect(()=>ctx.theme.register({id:'fixture-night',colorScheme:'dark',tokens:{'--dsw-alias-brand-primary':'#39ddaa','--dsw-alias-bg-base':'#112233','--dsw-alias-label-primary':'#eeeeee'}}))}")
 rows=ok('extensions.import',path=str(theme_plugin));theme_row=next(x for x in rows if x['package']=='dsh-theme-fixture');theme_ref=dict(source=theme_row['source'],package=theme_row['package'],version=theme_row['version'])
 ok('extensions.bind',**theme_ref,enabled=True,acceptHostPermissions=True)
 themes=ok('extensions.themes');assert any(t['name']=='fixture-night' and t['accent']=='#39ddaa' and t['tokens']['background']=='#112233' for t in themes),themes
 ok('extensions.bind',**theme_ref,enabled=False)
 assert not any(t['name']=='fixture-night' for t in ok('extensions.themes'))
 ok('extensions.bind',**theme_ref,enabled=True,acceptHostPermissions=True)
 assert any(t['name']=='fixture-night' for t in ok('extensions.themes'))
 ok('compat.disable');assert not ok('compat.status')['enabled']
 print('PASS default-off install barrier, real environment reuse/generation, immutable TS install, activation/UI/command, automatic quarantine, durable log and disable')
 # Python can run independently while DSH is off.
 ok('python.configure',executable=sys.executable);assert ok('python.status')['healthy'];p=root/'python';p.mkdir();manifest.update(packageId='python-fixture',engines={'dunlin':'desktop@1','host':'python'},entry='main.py');(p/'dunlin.json').write_text(json.dumps(manifest));effect=root/'python-effect.txt';(p/'main.py').write_text("from pathlib import Path\ndef activate(api,config):\n def write(params):\n  Path("+repr(str(effect))+").write_text(params['text']);return {'written':True}\n api.register_command('write',write)\n def fail(params):\n  raise ValueError('private exception message')\n api.register_command('fail',fail)\n api.contribute('action',{'kind':'command','title':'Write fixture','command':'write'})\n")
 rows=ok('extensions.import',path=str(p));row=next(x for x in rows if x['package']=='python-fixture');ref=dict(source=row['source'],package=row['package'],version=row['version']);ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True);r=ok('extensions.command',source=ref['source'],package=ref['package'],command='write',arguments={'text':'owned effect'});assert effect.read_text()=='owned effect'
 bad('extensions.command',source=ref['source'],package=ref['package'],command='fail',arguments={})
 diagnostic=ok('extensions.faults')[0].get('diagnostic','');assert 'ValueError' in diagnostic and 'main.py:' in diagnostic,diagnostic
 assert 'private exception message' not in diagnostic
 assert not next(x for x in ok('extensions.list') if x['package']=='python-fixture')['active']
 bad('extensions.command',source=ref['source'],package=ref['package'],command='write',arguments={'text':'must not run'});assert effect.read_text()=='owned effect'
 ok('extensions.recover',**ref)
 ok('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)
 ok('extensions.bind',**ref,enabled=False)
 theme=root/'theme';theme.mkdir();theme_manifest=dict(manifest,packageId='native-theme',kind='theme',engines={'dunlin':'desktop@1','host':'native-theme'},theme={'scheme':'dark','tokens':{'accent':'#7755ff'}});(theme/'dunlin.json').write_text(json.dumps(theme_manifest));rows=ok('extensions.import',path=str(theme));assert any(t['accent']=='#7755ff' for t in ok('extensions.themes'))
 # Added unlisted content must fail immutable package verification as well.
 row=next(x for x in rows if x['package']=='python-fixture');ref=dict(source=row['source'],package=row['package'],version=row['version']);sql=sqlite3.connect(private/'conversations.sqlite');installed=pathlib.Path(sql.execute('SELECT path FROM extension_versions WHERE source=? AND package=? AND version=?',(ref['source'],ref['package'],ref['version'])).fetchone()[0]);(installed/'unlisted.py').write_text('added after install');assert bad('extensions.bind',**ref,enabled=True,acceptHostPermissions=True)['error']=='extension_integrity_failed'
 assert sql.execute("SELECT count(*) FROM extension_calls WHERE status='reported'").fetchone()[0]>=2;sql.close();lib.dc_close(db);db=lib.dc_open(str(private).encode());assert db;assert not ok('compat.status')['enabled'];assert ok('extensions.list');lib.dc_close(db)
 print('PASS independent Python configure/import/enable, actual file effect, disable prevents execution, durable calls and restart inventory')
