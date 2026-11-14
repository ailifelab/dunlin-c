#!/usr/bin/env python3
"""Same-domain graph/RAG/memory transactions, exact evidence, scope and source deletion."""
import desktop_fixture_sqlite as fixture_sqlite
import ctypes as C,json,tempfile,pathlib,sys,sqlite3,hashlib
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_bind_identity',[P,S],C.c_int),('dc_knowledge_call',[P,S],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_delete_chat',[P,S],C.c_int)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
with tempfile.TemporaryDirectory(prefix='dunlin-knowledge-') as tmp:
 root=pathlib.Path(tmp);a=root/'a';b=root/'b';a.mkdir(mode=0o700);b.mkdir(mode=0o700);work=root/'work';work.mkdir();db=lib.dc_open(str(a).encode());assert db
 def req(op,**kw):
  raw=lib.dc_knowledge_call(db,json.dumps(dict(op=op,**kw),ensure_ascii=False).encode());assert raw
  try:return json.loads(C.string_at(raw))
  finally:lib.dc_free(raw)
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r.get('data')
 def bad(op,**kw):
  r=req(op,**kw);assert not r['ok'],r;return r
 assert bad('memory.list',scope='global',scopeID='')['error']=='identity_not_bound';assert lib.dc_bind_identity(db,b'alice');assert not lib.dc_bind_identity(db,b'bob')
 assert lib.dc_add_chat(db,b'chat',b'Chat',str(work).encode());source='不要自动升级 Node；只有我确认后才允许。';hash=hashlib.sha256(source.encode()).hexdigest();state={'active':False,'messages':[{'role':'user','content':source},{'role':'assistant','content':'assistant statement'}]};sql=fixture_sqlite.connect(a/'conversations.sqlite');sql.execute("UPDATE chats SET state=? WHERE id='chat'",(json.dumps(state),));sql.commit()
 memory=dict(id='m1',text='不要自动升级 Node，只有本人确认后才允许。',subject='alice',kind='preference',scope='global',scopeID='',sourceChat='chat',sourceSequence=0,sourceHash=hash)
 bad('memory.create',**dict(memory,id='wrong',sourceHash='00'*32));m=ok('memory.create',**memory);assert m['body']['status']=='proposed' and m['body']['speaker']=='alice'
 assert ok('knowledge.search',scope='global',scopeID='',query='升级')==[]
 m=ok('memory.update',id=m['id'],revision=m['revision'],status='confirmed');hits=ok('knowledge.search',scope='global',scopeID='',query='升级');assert hits[0]['body']['text'].startswith('不要')
 evidence=ok('graph.neighbors',id='m1',type='derivedFrom');assert len(evidence)==1 and evidence[0]['body']['quote']==source
 bad('memory.update',id='m1',revision=1,status='confirmed');m=ok('memory.update',id='m1',revision=m['revision'],status='confirmed',text='更正：只在测试环境中允许升级。');assert sql.execute("SELECT count(*) FROM graph_node_history WHERE node='m1'").fetchone()[0]==2
 ok('memory.subject.add',id='bob',name='Bob');assistant_hash=hashlib.sha256(b'assistant statement').hexdigest();m2=ok('memory.create',**dict(memory,id='m2',subject='bob',sourceSequence=1,sourceHash=assistant_hash));assert m2['body']['speaker']=='agent' and m2['body']['subject']=='bob' and m2['body']['speakerType']=='assistant'
 doc=ok('knowledge.add',id='manual',name='C 开发手册',text=('SQLite 图关系与事务检索。不要把关键词当作完整事实。'*120),scope='global',scopeID='');hits=ok('knowledge.search',scope='global',scopeID='',query='图关系');assert any(h['kind']=='chunk' for h in hits)
 for h in hits:
  if h['kind']=='chunk':assert doc['body']['text'].encode()[h['body']['start']:h['body']['end']].decode()==h['body']['text']
 assert ok('knowledge.list',scope='all',scopeID='')['rows'][0]['id']=='manual'
 bad('knowledge.move',id='manual',revision=doc['revision'],scope='chat',scopeID='chat')
 sql.execute("DELETE FROM knowledge_fts WHERE node IN(SELECT id FROM graph_nodes WHERE kind='chunk')");sql.commit()
 assert not any(h['kind']=='chunk' for h in ok('knowledge.search',scope='global',scopeID='',query='图关系'))
 ok('knowledge.reindex',id='manual',revision=doc['revision'])
 assert any(h['kind']=='chunk' for h in ok('knowledge.search',scope='global',scopeID='',query='图关系'))
 lib.dc_workspace_request.argtypes=[P,S];lib.dc_workspace_request.restype=P
 p=lib.dc_workspace_request(db,json.dumps(dict(op='projects.create',id='p1',name='Fixture',path=str(work),instructions='',model='',policy='noTools',context=[],bookmark='')).encode())
 assert p
 try: assert json.loads(C.string_at(p))['ok']
 finally:lib.dc_free(p)
 changed=ok('knowledge.move',id='manual',revision=doc['revision'],scope='project',scopeID='p1')
 bad('knowledge.move',id='manual',revision=doc['revision'],scope='global',scopeID='')
 assert not any(h['kind']=='chunk' for h in ok('knowledge.search',scope='global',scopeID='',query='图关系'))
 assert any(h['kind']=='chunk' for h in ok('knowledge.search',scope='project',scopeID='p1',query='图关系'))
 doc=ok('knowledge.move',id='manual',revision=changed['revision'],scope='global',scopeID='')
 print('PASS document index repaired from local sources, document and chunks change scope atomically, invalid scope and stale revision rejected')
 p=ok('knowledge.list',scope='global',scopeID='',limit=1);assert p['rows'][0]['id']=='manual' and not p['hasMore'];ok('knowledge.remove',id='manual',revision=doc['revision']);assert not any(h['kind']=='chunk' for h in ok('knowledge.search',scope='global',scopeID='',query='图关系'))
 # A user can keep a project/global memory in its existing scope while deleting its source.
 assert lib.dc_add_chat(db,b'keep',b'Keep',str(work).encode())
 sql.execute("UPDATE chats SET state=? WHERE id='keep'",(json.dumps(state),));sql.commit()
 keep=ok('memory.create',**dict(memory,id='keep-memory',sourceChat='keep'))
 ok('memory.update',id=keep['id'],revision=keep['revision'],status='confirmed')
 lib.dc_delete_chat_preserving_memories.argtypes=[P,S,C.c_int];lib.dc_delete_chat_preserving_memories.restype=C.c_int
 assert lib.dc_delete_chat_preserving_memories(db,b'keep',1)
 surviving=ok('graph.neighbors',id='keep-memory',type='derivedFrom')
 assert len(surviving)==1 and surviving[0]['body']['quote']==source
 keep=next(row for row in ok('memory.list',scope='global',scopeID='')['rows'] if row['id']=='keep-memory')
 assert keep['independent']
 ok('knowledge.remove',id=keep['id'],revision=keep['revision'])
 assert lib.dc_delete_chat(db,b'chat');assert ok('memory.list',scope='global',scopeID='')['rows']==[];assert ok('knowledge.search',scope='global',scopeID='',query='升级')==[]
 sql.close();lib.dc_close(db);db=lib.dc_open(str(b).encode());assert db;assert lib.dc_bind_identity(db,b'bob');assert not ok('knowledge.search',scope='global',scopeID='',query='升级');lib.dc_close(db)
 db=lib.dc_open(str(a).encode());assert db;assert lib.dc_bind_identity(db,b'alice');assert ok('memory.subjects');lib.dc_close(db)
 print('PASS trusted principal binding, proposed/confirmed separation, speaker versus subject, source hash/quote, correction history/CAS, CJK RAG chunk provenance, paging, deletion propagation and identity isolation')
