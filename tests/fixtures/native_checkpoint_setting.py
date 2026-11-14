"""Prepare one C-owned setting in the named disposable native acceptance store.
The native app must be closed. No model/tool calls or credentials are accessed.
"""
import ctypes as C,sys,json,pathlib,hashlib
root=pathlib.Path.cwd();library=pathlib.Path(sys.argv[1]).resolve();data=pathlib.Path(sys.argv[2]).resolve();assets=pathlib.Path(sys.argv[3]).resolve();report=pathlib.Path(sys.argv[4]);assert data==root/'build/v51/react-workbenches-ui/state46/data';assert library.is_relative_to(root/'build') and assets.is_relative_to(root/'build')
lib=C.CDLL(str(library));P=C.c_void_p;S=C.c_char_p
for name,args,ret in [('dc_open',[S],P),('dc_close',[P],None),('dc_extensions_call',[P,S,P],P),('dc_free',[P],None)]:f=getattr(lib,name);f.argtypes=args;f.restype=ret
store=lib.dc_open(str(data).encode());assert store

def call(op,**args):
 raw=lib.dc_extensions_call(store,json.dumps(dict(op=op,**args)).encode(),None);assert raw
 try:value=json.loads(C.string_at(raw))
 finally:lib.dc_free(raw)
 assert value['ok'],value
 return value.get('data')
try:
 call('extensions.assets',path=str(assets));call('compat.enable',node='/opt/homebrew/bin/node');restored=call('extensions.restore');assert restored['failed']==0
 rows=call('extensions.list');row=next(r for r in rows if r['package']=='dsh-checkpoint-rewind');assert row['active'];ref={k:row[k]for k in ['source','package','version']};before=call('extensions.config.get',**ref);config=before['config'];assert config['listLimit']==10;config['listLimit']=7
 call('extensions.settings.apply',**ref,chat='ui-chat',expectedHash=before['hash'],config=config);after=call('extensions.config.get',**ref);assert after['config']['listLimit']==7;assert not call('extensions.faults')
 report.write_text(json.dumps({'scope':'Owned closed native state46; C settings preparation only','from':10,'to':7,'hash':after['hash'],'library':str(library),'librarySHA':hashlib.sha256(library.read_bytes()).hexdigest(),'noModelOrToolCalls':True},indent=2)+'\n');print('PASS owned C configuration10 ->7, no model/tool')
finally:
 try:call('extensions.quiesce')
 finally:lib.dc_close(store)
