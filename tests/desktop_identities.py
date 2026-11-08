#!/usr/bin/env python3
"""C-owned local identity routing: actual separate stores, no UI-only filtering."""
import ctypes as C,json,pathlib,tempfile,sys
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for name,args,ret in [('dc_identity_open',[S],P),('dc_identity_close',[P],None),('dc_identity_request',[P,S],P),('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_chats',[P],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_workspace_request',[P,S],P)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=ret
lib.dc_sha256.argtypes=[P,C.c_size_t,P]
def value(raw):
 assert raw
 try:return json.loads(C.string_at(raw))
 finally:lib.dc_free(raw)
import hashlib
for data in [b'',b'abc',b'x'*56,b'x'*63,b'x'*64,b'x'*1000000,'保持语义。'.encode()]:
 buf=C.create_string_buffer(65);lib.dc_sha256(data,len(data),buf);assert buf.value.decode()==hashlib.sha256(data).hexdigest()
print('PASS portable SHA-256 standard/block-boundary/large/UTF8 vectors')
with tempfile.TemporaryDirectory(prefix='dunlin-identities-') as tmp:
 root=pathlib.Path(tmp);root.chmod(0o700);catalog=lib.dc_identity_open(str(root).encode());assert catalog;assert not lib.dc_identity_open(str(root).encode())
 def req(op,**kw):return value(lib.dc_identity_request(catalog,json.dumps(dict(op=op,**kw)).encode()))
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r.get('data')
 def bad(op,**kw):assert not req(op,**kw)['ok']
 rows=ok('list');assert len(rows)==1 and rows[0]['id']=='local' and rows[0]['selected'];epoch=rows[0]['epoch']
 a=lib.dc_open(rows[0]['directory'].encode());assert a;assert lib.dc_add_chat(a,b'private-a',b'A private transcript',str(root).encode())
 ok('create',id='person-b',name='第二身份',kind='person',members=[])
 bad('create',id='../escape',name='bad',kind='person',members=[])
 bad('create',id='same-name',name='第二身份',kind='person',members=[])
 bad('create',id='bad-member',name='bad member',kind='shared',members=['unknown'])
 rows=ok('create',id='family',name='家庭共有',kind='shared',members=['local','person-b']);brow=next(x for x in rows if x['id']=='person-b');b=lib.dc_open(brow['directory'].encode());assert b
 assert value(lib.dc_chats(b))==[];assert lib.dc_add_chat(b,b'private-b',b'B private transcript',str(root).encode())
 rows=ok('select',id='person-b',epoch=epoch);epoch+=1;assert next(x for x in rows if x['selected'])['id']=='person-b'
 bad('select',id='local',epoch=epoch-1);bad('remove',id='person-b');bad('remove',id='local')
 print('PASS identity selection epoch CAS; separate authoritative private chat domains; shared membership validates')
 fam=next(x for x in rows if x['id']=='family');f=lib.dc_open(fam['directory'].encode());assert f;assert value(lib.dc_chats(f))==[]
 assert len(value(lib.dc_chats(a)))==len(value(lib.dc_chats(b)))==1
 ok('remove',id='family');assert (root/'Identities/family').is_dir();ok('restore',id='family')
 lib.dc_close(a);lib.dc_close(b);lib.dc_close(f);lib.dc_identity_close(catalog)
 catalog=lib.dc_identity_open(str(root).encode());assert catalog;rows=ok('list');assert next(x for x in rows if x['selected'])['id']=='person-b';lib.dc_identity_close(catalog)
 print('PASS shared identity starts empty; reversible removal; selected identity persists on restart')
