#!/usr/bin/env python3
"""Derived-index lifecycle; independent fixture state, no network or models."""
import desktop_fixture_sqlite as fixture_sqlite
import ctypes as C,json,sys,pathlib,tempfile,sqlite3,hashlib,random
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for name,args,ret in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_delete_chat',[P,S],C.c_int),('dc_workspace_request',[P,S],P),('dc_search',[P,S],P),('dc_index_pending',[P,C.c_uint],C.c_int)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=ret
def val(raw):
 assert raw
 try:return json.loads(C.string_at(raw))
 finally:lib.dc_free(raw)
with tempfile.TemporaryDirectory(prefix='dunlin-index-') as tmp:
 root=pathlib.Path(tmp);private=root/'private';private.mkdir(mode=0o700);work=root/'work';work.mkdir();db=lib.dc_open(str(private).encode());assert db
 def req(op,**kw):return val(lib.dc_workspace_request(db,json.dumps(dict(op=op,**kw)).encode()))
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r['data']
 assert lib.dc_add_chat(db,b'c1',b'Chat',str(work).encode())
 text='不要删除 src/v5.1 文件。只有我确认后才能执行。';state={'active':False,'messages':[{'role':'user','content':text},{'role':'assistant','content':'已记录本次要求。'}]}
 sql=fixture_sqlite.connect(private/'conversations.sqlite');sql.execute("UPDATE chats SET state=? WHERE id='c1'",(json.dumps(state),));sql.execute('INSERT INTO index_jobs(chat,seq,text) VALUES(?,?,?)',('c1',0,text));sql.commit()
 assert ok('index.pause',value=True)['paused'];assert lib.dc_index_pending(db,8)==0;assert ok('index.status')['pending']==1
 assert not val(lib.dc_search(db,'删除'.encode()))
 assert not ok('index.pause',value=False)['paused'];assert lib.dc_index_pending(db,8)==0
 status=ok('index.status');assert status['pending']==0 and status['prepared']==1 and status['documents']==1
 hit=val(lib.dc_search(db,'删除文件'.encode()));assert hit and hit[0]['chat']=='c1'
 digest,spans=sql.execute('SELECT source_hash,spans FROM turn_preparation').fetchone();assert digest==hashlib.sha256(text.encode()).hexdigest();spans=json.loads(spans);assert len(spans)==2
 for span in spans:assert text.encode()[span['start']:span['end']].decode()
 before=sql.execute('SELECT state FROM chats').fetchone()[0]
 ok('index.rebuild');assert ok('index.status')['pending']==2
 assert lib.dc_index_pending(db,8)==0;assert ok('index.status')['documents']==2
 assert sql.execute('SELECT state FROM chats').fetchone()[0]==before
 assert not req('index.quota',bytes=1000)['ok'];assert ok('index.quota',bytes=1048576)['quota']==1048576
 # Fill only derived data; quota must preserve failed outbox rows and originals.
 rng=random.Random(51)
 for i in range(80):sql.execute('INSERT INTO index_jobs(chat,seq,text) VALUES(?,?,?)',('c1',100+i,' '.join('%016x'%rng.getrandbits(64) for _ in range(4000))))
 sql.commit();result=lib.dc_index_pending(db,100);status=ok('index.status');print('quota result',result,status,flush=True);assert result==-1;assert status['fault']==2 and status['pending']>0
 assert sql.execute('SELECT state FROM chats').fetchone()[0]==before
 sql.close();lib.dc_close(db)
 # A corrupt derived DB must not prevent source-store open or destroy messages.
 (private/'history.sqlite').write_bytes(b'not a sqlite database')
 db=lib.dc_open(str(private).encode());assert db;assert not ok('index.status')['available'];assert lib.dc_index_pending(db,8)==-1
 ok('index.rebuild');assert lib.dc_index_pending(db,8)==0;assert val(lib.dc_search(db,'删除'.encode()))
 assert lib.dc_delete_chat(db,b'c1');assert not val(lib.dc_search(db,'删除'.encode()));assert ok('index.status')['prepared']==0
 lib.dc_close(db)
 print('PASS pause/resume, exact-span cache, source-preserving rebuild, quota failure/outbox retention, corrupt-cache recovery and delete cleanup')
