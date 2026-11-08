#!/usr/bin/env python3
"""Corrupt local reader fails closed, without partial fabricated message DTOs."""
import desktop_fixture_sqlite as fixture_sqlite
import ctypes as C,json,pathlib,sqlite3,sys,tempfile
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));lib.dc_open.argtypes=[C.c_char_p];lib.dc_open.restype=C.c_void_p;lib.dc_add_chat.argtypes=[C.c_void_p,C.c_char_p,C.c_char_p,C.c_char_p];lib.dc_add_chat.restype=C.c_int;lib.dc_messages.argtypes=[C.c_void_p,C.c_char_p];lib.dc_messages.restype=C.c_void_p;lib.dc_free.argtypes=[C.c_void_p];lib.dc_close.argtypes=[C.c_void_p]
with tempfile.TemporaryDirectory(prefix='dunlin-malformed-messages-') as tmp:
 root=pathlib.Path(tmp);root.chmod(0o700);db=lib.dc_open(str(root).encode());assert db and lib.dc_add_chat(db,b'chat',b'Owned corrupt fixture',str(root).encode())
 cases=[dict(messages={}),dict(messages=[dict(role='unknown',content='body')]),dict(messages=[dict(role='user',content={})]),dict(messages=[42]),dict(messages=[None]),dict(messages=[dict(role=1,content='body')]),dict(messages=[dict(role='assistant\x00other',content='body')]),dict(messages=[dict(role='assistant',content='prefix\x00suffix')]),[],dict(messages='bad')]
 for index,source in enumerate(cases):
  with fixture_sqlite.connect(root/'conversations.sqlite') as sql:sql.execute("UPDATE chats SET state=? WHERE id='chat'",(json.dumps(source),))
  print('Checking malformed source',index,flush=True)
  result=lib.dc_messages(db,b'chat')
  if result:
   text=C.string_at(result);lib.dc_free(result);raise AssertionError(('corrupt source returned misleading messages',index,text))
 with fixture_sqlite.connect(root/'conversations.sqlite') as sql:sql.execute("UPDATE chats SET state=? WHERE id='chat'",(json.dumps(dict(messages=[])),))
 result=lib.dc_messages(db,b'chat');assert result and C.string_at(result)==b'[]';lib.dc_free(result);lib.dc_close(db)
 print('PASS malformed container/message/role/NUL rejects the entire read; valid empty source remains readable')
