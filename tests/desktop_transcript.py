#!/usr/bin/env python3
"""Portable source-text backups never replay actions or overwrite an existing chat."""
import ctypes
import json
import pathlib
import sqlite3
import sys
import tempfile

lib = ctypes.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
string, pointer, integer = ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int
for name, args, result in [
    ("dc_open", [string], pointer), ("dc_bind_identity", [pointer, string], integer),
    ("dc_close", [pointer], None), ("dc_free", [pointer], None),
    ("dc_transcript_export", [pointer, string], pointer), ("dc_transcript_preview", [string], pointer),
    ("dc_transcript_restore", [pointer, string, string, string], integer),
    ("dc_messages", [pointer, string], pointer),
]:
    f = getattr(lib, name)
    f.argtypes, f.restype = args, result

def decode(p):
    assert p
    try:
        return json.loads(ctypes.string_at(p))
    finally:
        lib.dc_free(p)

def encode(value):
    return json.dumps(value, ensure_ascii=False).encode()

with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    ds = lib.dc_open(bytes(root))
    assert ds and lib.dc_bind_identity(ds, b"alice")
    source = dict(title="保留原文", messages=[
        dict(role="user", content="Please keep the punctuation!"),
        dict(role="assistant", content="第一段。\n第二段。")
    ])
    raw = encode(source)
    assert decode(lib.dc_transcript_preview(raw)) == {"title": "保留原文", "messages": 2}
    for role in ["system", "tool", "developer"]:
        assert not lib.dc_transcript_preview(encode(dict(source, messages=[dict(role=role, content="foreign")])))
    assert not lib.dc_transcript_preview(encode(dict(source, active=True)))
    assert not lib.dc_transcript_preview(encode(dict(source, messages=[dict(source["messages"][1], tool_calls=[])])))
    assert not lib.dc_transcript_preview(encode(dict(source, messages=[dict(role="user", content="a\0b")])))
    assert not lib.dc_transcript_preview(encode(dict(source, messages=[dict(role="user", content="x" * 1048577)])))
    assert not lib.dc_transcript_restore(ds, b"foreign", b"another", raw)
    assert lib.dc_transcript_restore(ds, b"restored", b"alice", raw)
    assert not lib.dc_transcript_restore(ds, b"restored", b"alice", raw)
    assert decode(lib.dc_transcript_export(ds, b"restored")) == source
    with sqlite3.connect(str(root / "conversations.sqlite")) as db:
        state, workspace = db.execute("SELECT state,workspace FROM chats WHERE id='restored'").fetchone()
        assert json.loads(state) == dict(version=1, rounds=0, active=False, messages=source["messages"])
        assert pathlib.Path(workspace).resolve() == root.resolve() / "Chat Workspace"
        assert db.execute("SELECT seq FROM index_jobs ORDER BY seq").fetchall() == [(0,), (1,)]
        db.execute("CREATE TRIGGER transcript_test_fault BEFORE INSERT ON index_jobs WHEN NEW.chat='fault' "
                   "BEGIN SELECT RAISE(ABORT,'fixture'); END")
        assert not lib.dc_transcript_restore(ds, b"fault", b"alice", raw)
        assert db.execute("SELECT id FROM chats ORDER BY id").fetchall() == [("restored",)]
        assert db.execute("SELECT count(*) FROM index_jobs WHERE chat='fault'").fetchone() == (0,)
    lib.dc_close(ds)
    ds = lib.dc_open(bytes(root))
    assert ds and lib.dc_bind_identity(ds, b"alice")
    assert [dict(role=m["role"], content=m["content"]) for m in decode(lib.dc_messages(ds, b"restored"))] == source["messages"]
    lib.dc_close(ds)
print("PASS transcript isolation, bounded role/text projection, duplicate protection, index rollback and reopen")
