#!/usr/bin/env python3
"""A deleted chat closes its local lease; remote cleanup remains reachable."""
import ctypes
import sqlite3
import sys
import tempfile
import time
from pathlib import Path

lib = ctypes.CDLL(sys.argv[1])
ptr, string, integer = ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int
for name, args, result in [
    ("dc_open", [string], ptr), ("dc_close", [ptr], None),
    ("dc_bind_identity", [ptr, string], integer),
    ("dc_add_chat", [ptr, string, string, string], integer),
    ("dc_delete_chat", [ptr, string], integer),
    ("dc_remote_account_record", [ptr, string, string, string, string, ctypes.c_int64], integer),
]:
    fn = getattr(lib, name)
    fn.argtypes, fn.restype = args, result

with tempfile.TemporaryDirectory(prefix="dunlin-drop-grants-") as temp:
    root = Path(temp)
    store = lib.dc_open(str(root).encode())
    assert store and lib.dc_bind_identity(store, b"local")
    try:
        for chat in (b"a", b"b"):
            assert lib.dc_add_chat(store, chat, chat, b"")
            assert lib.dc_remote_account_record(store, chat, chat, b"test",
                                                b"https://example.invalid/revoke", int(time.time()) + 300)
        with sqlite3.connect(root / "conversations.sqlite") as db:
            states = lambda: dict(db.execute("SELECT id,state FROM capability_leases ORDER BY id"))
            assert states() == {"a": "active", "b": "active"}
            db.execute("CREATE TRIGGER fixture_failure BEFORE DELETE ON chats BEGIN SELECT RAISE(ABORT,'fixture failure'); END")
            db.commit()
            assert not lib.dc_delete_chat(store, b"a")
            assert states() == {"a": "active", "b": "active"}
            db.execute("DROP TRIGGER fixture_failure")
            db.commit()
            assert lib.dc_delete_chat(store, b"a")
            assert states() == {"a": "expired", "b": "active"}, states()
            assert db.execute("SELECT count(*) FROM remote_grant_endpoints WHERE id='a'").fetchone() == (1,)
    finally:
        lib.dc_close(store)
print("PASS failed deletion leaves leases in place; committed deletion expires only its local lease and preserves remote revocation records")
