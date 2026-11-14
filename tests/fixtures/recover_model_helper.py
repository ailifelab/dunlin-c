#!/usr/bin/env python3
"""Exclusive C startup recovery of a named, killed test owner's private store."""
import ctypes as C,json,pathlib,sys,sqlite3,hashlib
library=pathlib.Path(sys.argv[1]).resolve();home=pathlib.Path(sys.argv[2]).resolve();nonce=sys.argv[3]
assert home.name.startswith('dunlin-react-owner-') and (home/'helper-crash-owned.json').read_text()==nonce
lib=C.CDLL(str(library));P=C.c_void_p;S=C.c_char_p
for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_helper_execute',[P,S,C.c_longlong,S,P],P),('dc_read_view',[S,S],P),('dc_extensions_call',[P,S,P],P)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=result
data=home/'data';db=lib.dc_open(str(data).encode());assert db
def value(pointer):
 assert pointer
 try:return json.loads(C.string_at(pointer))
 finally:lib.dc_free(pointer)
def extension(op,**fields):return value(lib.dc_extensions_call(db,json.dumps(dict(op=op,**fields)).encode(),None))
try:
 with sqlite3.connect((data/'conversations.sqlite').as_uri()+'?mode=ro',uri=True) as sql:
  jobs=[dict(zip(['id','call','state','error','revision'],row)) for row in sql.execute('SELECT j.id,j.call,j.state,j.error,p.revision FROM model_helper_jobs j JOIN model_helper_plans p ON p.call=j.call')]
  outer=[dict(zip(['id','status'],row)) for row in sql.execute("SELECT id,status FROM extension_calls WHERE id IN(SELECT call FROM model_helper_plans)")]
  parent=sql.execute("SELECT state FROM chats WHERE id='ui-chat'").fetchone()[0]
 assert len(jobs)==1 and jobs[0]['state']=='unknown',jobs
 assert outer==[{'id':jobs[0]['call'],'status':'unknown'}],outer
 pending=value(lib.dc_read_view(str(data).encode(),b'{"op":"helpers.pending"}'));assert pending['ok'] and pending['data']==[]
 denied=value(lib.dc_helper_execute(db,jobs[0]['id'].encode(),jobs[0]['revision'],b'',None));assert not denied['ok'],denied
 rows=extension('extensions.list');assert rows['ok'];item=next(x for x in rows['data'] if x['package']=='dsh-project-manager');assert not item['enabled'] and item['versions'][0]['quarantined']
 faults=extension('extensions.faults');assert faults['ok'] and any(f['package']=='dsh-project-manager' and f['phase']=='recovery' for f in faults['data'])
 print(json.dumps({'jobs':jobs,'outer':outer,'pending':pending,'reexecution':denied,'parentSHA':hashlib.sha256(parent.encode()).hexdigest(),'ownerDisabled':True,'faults':faults['data']},ensure_ascii=False),flush=True)
finally:lib.dc_close(db)
