#!/usr/bin/env python3
"""Derived-index lifecycle; independent fixture state, no network or models."""
import desktop_fixture_sqlite as fixture_sqlite
import ctypes as C,json,sys,pathlib,tempfile,sqlite3,hashlib,random
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for name,args,ret in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_delete_chat',[P,S],C.c_int),('dc_workspace_request',[P,S],P),('dc_search',[P,S],P),('dc_search_filtered',[P,S,S,C.c_int64],P),('dc_index_pending',[P,C.c_uint],C.c_int)]:
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
 hit=val(lib.dc_search(db,'删除文件'.encode()));assert hit and hit[0]['chat']=='c1' and hit[0]['role']=='user' and 'time' not in hit[0] and '[' in hit[0]['matchSnippet']
 digest,spans=sql.execute('SELECT source_hash,spans FROM turn_preparation').fetchone();assert digest==hashlib.sha256(text.encode()).hexdigest();spans=json.loads(spans);assert len(spans)==2
 for span in spans:assert text.encode()[span['start']:span['end']].decode()
 before=sql.execute('SELECT state FROM chats').fetchone()[0]
 excerpt=ok('index.source',chat='c1',sequence=0,text=text)
 assert [row['text'] for row in excerpt]==[text,'已记录本次要求。'] and all('time' not in row for row in excerpt)
 assert not req('index.source',chat='c1',sequence=0,text='outdated')['ok']
 assert not req('index.source',chat='c1',sequence=0,text=text,role='assistant')['ok']
 assert not req('index.source',chat='c1',sequence=0,text=text,time=123)['ok']
 assert not req('index.source',chat='missing',sequence=0,text=text)['ok']
 # Tool frames can span several ordinals between nearby human-visible messages.
 frames=[{'role':'assistant','content':'准备升级。'},{'role':'system','content':'internal'},
         {'role':'user','content':text},{'role':'assistant','content':None},
         {'role':'tool','content':'tool-only output'},{'role':'tool','content':'more output'},
         {'role':'assistant','content':'收到。'},{'role':'user','content':'但测试环境可以自动升级。'}]
 sql.execute("UPDATE chats SET state=? WHERE id='c1'",(json.dumps({'messages':frames}),));sql.commit()
 adjacent=ok('index.source',chat='c1',sequence=2,text=text,role='user')
 assert [row['sequence'] for row in adjacent]==[0,2,6,7],adjacent
 sql.execute("UPDATE chats SET state=? WHERE id='c1'",(before,));sql.commit()
 cleared=ok('index.clear')
 assert cleared['paused'] and all(cleared[k]==0 for k in ('pending','pendingDeletes','documents','prepared'))
 assert not val(lib.dc_search(db,'删除文件'.encode()))
 assert sql.execute('SELECT state FROM chats').fetchone()[0]==before
 lib.dc_close(db);db=lib.dc_open(str(private).encode());assert db and ok('index.status')['paused']
 assert not ok('index.rebuild')['paused'];assert ok('index.status')['pending']==2
 assert lib.dc_index_pending(db,8)==0;assert ok('index.status')['documents']==2
 assert sql.execute('SELECT state FROM chats').fetchone()[0]==before
 assert ok('index.status')['automaticQuota']
 assert not req('index.quota',bytes=1000)['ok'];assert ok('index.quota',bytes=1048576)['quota']==1048576
 assert not ok('index.status')['automaticQuota'];automatic=ok('index.quota',bytes=0)
 assert automatic['automaticQuota'] and automatic['quota']==67108864
 ok('index.quota',bytes=1048576)
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
 # The first 50 equal-ranked hits belong to an unrelated project. Filtering
 # must continue into later pages and use message time, never chat update time.
 assert lib.dc_add_chat(db,b'other',b'Other',str(work).encode())
 assert lib.dc_add_chat(db,b'target',b'Target',str(work).encode())
 with fixture_sqlite.connect(private/'conversations.sqlite') as writer:
  for chat,count in [('other',60),('target',2)]:
   rows=[{'role':'user','content':'needle'} for _ in range(count)]
   rows[-1]['_dunlin_time']=2000
   writer.execute("UPDATE chats SET state=? WHERE id=?",(json.dumps({'messages':rows}),chat))
   writer.execute("INSERT OR IGNORE INTO chat_properties(chat) VALUES(?)",(chat,))
   writer.execute("UPDATE chat_properties SET project=? WHERE chat=?",(chat,chat))
   writer.executemany('INSERT INTO index_jobs(chat,seq,text) VALUES(?,?,?)',[(chat,n,'needle') for n in range(count)])
 assert lib.dc_index_pending(db,100)==0
 assert [r['sequence'] for r in val(lib.dc_search_filtered(db,b'needle',b'target',0))]==[0,1]
 timed=val(lib.dc_search_filtered(db,b'needle',b'target',2000));assert [(r['sequence'],r['role'],r['time']) for r in timed]==[(1,'user',2000)]
 assert not val(lib.dc_search_filtered(db,b'needle',b'target',2001))
 # Multimodal turns index their user-visible caption via the same rule on rebuild.
 with fixture_sqlite.connect(private/'conversations.sqlite') as writer:
  writer.execute("UPDATE chats SET state=? WHERE id='target'",(json.dumps({'messages':[{'role':'user','content':[{'type':'text','text':'provider envelope'}],'_dunlin_display':'photo caption'}]}),))
 ok('index.rebuild')
 assert lib.dc_index_pending(db,100)==0
 assert val(lib.dc_search(db,b'caption'))[0]['text']=='photo caption'
 assert not val(lib.dc_search(db,b'envelope'))
 lib.dc_close(db)
 print('PASS pause/resume, exact-span cache, source-preserving rebuild, quota failure/outbox retention, corrupt-cache recovery and delete cleanup')
