#!/usr/bin/env python3
"""Optional data-only RPC adapter for trusted Python extensions, not a sandbox."""
import os,sys,json,importlib.util,traceback
from pathlib import Path
output=sys.stdout
sys.stdout=sys.stderr
plugins={};commands={};contributions={}
def write(value):output.write(json.dumps(value,ensure_ascii=False,separators=(',',':'))+'\n');output.flush()
def descriptor_valid(d):
 if not isinstance(d,dict) or d.get('kind') not in ['command','settings','sidebar','theme']:return False
 def text(v,limit):return isinstance(v,str) and bool(v) and len(v.encode())<=limit and '\0' not in v
 for key in ['title','name','body','command','scheme']:
  if key in d and not text(d[key],16384 if key=='body' else 256):return False
 for key,fields in [('actions',['id','title','command']),('sections',['id','title','body'])]:
  if key not in d:continue
  if not isinstance(d[key],list) or len(d[key])>32:return False
  ids=set()
  for row in d[key]:
   if not isinstance(row,dict) or any(not text(row.get(k),8192 if k=='body' else 256) for k in fields) or row['id'] in ids:return False
   ids.add(row['id'])
 return d['kind']!='command' or text(d.get('command'),128)
class API:
 def __init__(self,owner):self.owner=owner
 def register_command(self,name,handler):
  if not isinstance(name,str) or len(name)>128 or not callable(handler):raise ValueError('invalid command')
  key=self.owner+':'+name
  if key in commands:raise ValueError('duplicate command')
  commands[key]=(self.owner,handler);return key
 def contribute(self,name,descriptor):
  if not descriptor_valid(descriptor) or not isinstance(name,str) or not name or len(name)>128:raise ValueError('invalid contribution')
  if len(json.dumps(descriptor).encode())>32768:raise ValueError('oversize contribution')
  key=self.owner+':'+name
  if key in contributions:raise ValueError('duplicate contribution')
  contributions[key]=dict(descriptor,id=key,plugin=self.owner);return key

def deactivate(params):
 owner=params['id'];plugin=plugins.pop(owner,None)
 try:
  if plugin is not None and callable(getattr(plugin,'dispose',None)):plugin.dispose()
 finally:
  for key in list(commands):
   if commands[key][0]==owner:del commands[key]
  for key in list(contributions):
   if contributions[key]['plugin']==owner:del contributions[key]
 return {'state':'inactive'}
def activate(params):
 owner=params['id'];root=Path(params['root']).resolve();entry=(root/params['entry']).resolve()
 if root not in entry.parents or owner in plugins:raise ValueError('invalid activation')
 spec=importlib.util.spec_from_file_location('dunlin_'+owner,entry)
 if spec is None or spec.loader is None:raise ValueError('invalid module')
 plugin=importlib.util.module_from_spec(spec)
 try:spec.loader.exec_module(plugin);plugin.activate(API(owner),params.get('config',{}));plugins[owner]=plugin
 except BaseException:deactivate({'id':owner});raise
 return {'id':owner,'state':'active'}
def command(params):
 owner,handler=commands.get(params['command'],(None,None))
 if owner!=params.get('plugin') or handler is None:raise ValueError('unknown command')
 return handler(params.get('arguments',{}))
methods={
 'system.info':lambda _:{'kind':'python','version':'.'.join(map(str,sys.version_info[:3])),'permissionModel':'trusted-full-host'},
 'plugins.activate':activate,'plugins.deactivate':deactivate,'plugins.command':command,
 'plugins.contributions':lambda _:list(contributions.values()),'plugins.list':lambda _:list(plugins)
}
write({'protocol':'dunlin.worker','major':1,'nonce':os.environ['DUNLIN_WORKER_NONCE'],'methods':list(methods),'runtime':{'kind':'python','version':'.'.join(map(str,sys.version_info[:3]))}})
for line in sys.stdin:
 if len(line.encode())>1048576:sys.exit(73)
 request=None
 try:
  request=json.loads(line)
  if request.get('jsonrpc')!='2.0' or not isinstance(request.get('id'),int) or request.get('method') not in methods:raise ValueError('invalid request')
  result=methods[request['method']](request.get('params',{}));write({'jsonrpc':'2.0','id':request['id'],'result':result})
 except BaseException as error:
  write({'jsonrpc':'2.0','id':request.get('id') if isinstance(request,dict) else None,'error':{'code':-32603,'message':'Plugin execution failed','diagnostic':'\n'.join([type(error).__name__[:128]]+[Path(frame.filename).name[:128]+':'+str(frame.lineno) for frame in traceback.extract_tb(error.__traceback__,limit=8)])[:2048]}})
for owner in list(plugins):deactivate({'id':owner})
