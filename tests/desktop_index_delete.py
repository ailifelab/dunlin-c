#!/usr/bin/env python3
"""Owned source/cache lock, ID reuse, crash-reopen and stale-hit regression.

No network/model or user data. The cache lock deliberately makes the real C
derived DELETE fail; canonical source deletion must remain safe and recoverable.
"""
import ctypes as C
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import desktop_fixture_sqlite as fixture

lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_delete_chat',[P,S],C.c_int),('dc_workspace_request',[P,S],P),('dc_search',[P,S],P),('dc_index_pending',[P,C.c_uint],C.c_int)]:
    f=getattr(lib,name);f.argtypes=args;f.restype=result

def value(p):
    assert p,'C response unavailable'
    try:return json.loads(C.string_at(p))
    finally:lib.dc_free(p)

with tempfile.TemporaryDirectory(prefix='dunlin-index-delete-') as tmp:
    root=Path(tmp);private=root/'private';private.mkdir(mode=0o700);work=root/'work';work.mkdir()
    db=lib.dc_open(str(private).encode());assert db
    source=fixture.connect(private/'conversations.sqlite')
    cache=sqlite3.connect(private/'history.sqlite',timeout=0)
    def request(op,**fields):
        r=value(lib.dc_workspace_request(db,json.dumps(dict(op=op,**fields)).encode()))
        assert r['ok'],r
        return r.get('data')
    def add(chat,text=None):
        assert lib.dc_add_chat(db,chat.encode(),chat.encode(),str(work).encode())
        if text is not None:seed(chat,text)
    def seed(chat,text,seq=0):
        source.execute('UPDATE chats SET state=? WHERE id=?',(json.dumps(dict(active=False,messages=[dict(role='user',content=text)]),ensure_ascii=False),chat))
        source.execute('INSERT OR REPLACE INTO index_jobs VALUES(?,?,?)',(chat,seq,text));source.commit()
    def search(text):return value(lib.dc_search(db,text.encode()))
    add('reused','OldCacheNeedle 删除前原文');add('other','OtherNeedle 保留')
    assert lib.dc_index_pending(db,16)==0
    assert len(search('OldCacheNeedle'))==1 and len(search('OtherNeedle'))==1
    cache.execute('BEGIN IMMEDIATE')
    assert lib.dc_delete_chat(db,b'reused'),'cache lock must not block source deletion'
    assert cache.execute("SELECT count(*) FROM history_fts WHERE chat='reused'").fetchone()[0]==1,'fixture must keep the stale cache'
    assert not search('OldCacheNeedle'),'deleted source must hide stale cache'
    add('reused')
    assert not search('OldCacheNeedle'),'ID reuse must not resurrect deleted content'
    cache.rollback()
    assert request('index.status')['pendingDeletes']==1
    seed('reused','NewCacheNeedle 新对话原文')
    cache.execute('BEGIN IMMEDIATE')
    assert lib.dc_index_pending(db,16)==-1,'failed cleanup must precede current jobs'
    assert request('index.status')['pendingDeletes']==1
    assert source.execute("SELECT count(*) FROM index_jobs WHERE chat='reused'").fetchone()[0]==1
    cache.rollback()
    lib.dc_close(db);db=lib.dc_open(str(private).encode());assert db
    # A restarted host retries the durable cleanup before indexing new content.
    assert lib.dc_index_pending(db,16)==0
    assert request('index.status')['pendingDeletes']==0
    assert request('index.status')['fault']==0
    assert not search('OldCacheNeedle') and len(search('NewCacheNeedle'))==1
    assert len(search('OtherNeedle'))==1
    print('PASS real cache-lock failure, canonical deletion, ID reuse hiding, pending-job preservation, restart cleanup and unrelated chat retention',flush=True)

    # Derived cache corruption must not become authoritative conversation text.
    cache.execute("UPDATE history_fts SET text='TamperedCacheNeedle',terms='tamperedcacheneedle' WHERE chat='reused'");cache.commit()
    assert not search('TamperedCacheNeedle'),'cache text must match current source'
    request('index.rebuild');assert lib.dc_index_pending(db,16)==0
    assert len(search('NewCacheNeedle'))==1
    source.execute("UPDATE chats SET state=? WHERE id='reused'",(json.dumps(dict(active=False,messages=[dict(role='user',content='SourceChangedNeedle')])),));source.commit()
    assert not search('NewCacheNeedle'),'source replacement must invalidate stale cache text'
    # Source-preserving display input is the authoritative searchable text.
    state=dict(active=False,messages=[dict(role='user',content='Expanded transport input',_dunlin_display='OriginalDisplayNeedle 原话')])
    source.execute("UPDATE chats SET state=? WHERE id='reused'",(json.dumps(state),));source.commit()
    request('index.rebuild');assert lib.dc_index_pending(db,16)==0
    assert len(search('OriginalDisplayNeedle'))==1 and not search('transport')
    # Canonical history can exceed a model request window; search must not
    # invent a smaller checkpoint limit than the authoritative store.
    state['messages'].extend(dict(role='assistant',content='x'*200000) for _ in range(25))
    source.execute("UPDATE chats SET state=? WHERE id='reused'",(json.dumps(state),));source.commit()
    assert len(search('OriginalDisplayNeedle'))==1
    print('PASS exact current canonical/display text validation and stale/corrupt derived result rejection',flush=True)
    # Rebuild atomically replaces the file. Lock the current cache, not the
    # old inode retained by this test's corruption connection.
    cache.close();cache=sqlite3.connect(private/'history.sqlite',timeout=0)

    # Source rollback must roll back its tombstone, not delete unrelated caches.
    source.execute("CREATE TRIGGER owned_delete_fault BEFORE DELETE ON chats WHEN OLD.id='other' BEGIN SELECT RAISE(ABORT,'owned failure'); END");source.commit()
    assert not lib.dc_delete_chat(db,b'other')
    assert source.execute("SELECT count(*) FROM index_deletions WHERE chat='other'").fetchone()[0]==0
    assert len(search('OtherNeedle'))==1
    source.execute('DROP TRIGGER owned_delete_fault');source.commit()
    source.execute("CREATE TRIGGER owned_cleanup_queue_fault BEFORE INSERT ON index_deletions WHEN NEW.chat='other' BEGIN SELECT RAISE(ABORT,'owned queue failure'); END");source.commit()
    assert not lib.dc_delete_chat(db,b'other'),'cleanup intent failure must roll back source deletion'
    assert len(search('OtherNeedle'))==1
    source.execute('DROP TRIGGER owned_cleanup_queue_fault');source.commit()
    request('index.pause',value=True)
    cache.execute('BEGIN IMMEDIATE');assert lib.dc_delete_chat(db,b'reused');cache.rollback()
    assert request('index.status')['pendingDeletes']==1
    assert lib.dc_index_pending(db,16)==0,'pause stops text processing, not deletion recovery'
    assert request('index.status')['pendingDeletes']==0 and request('index.status')['paused']
    assert cache.execute("SELECT count(*) FROM history_fts WHERE chat='reused'").fetchone()[0]==0
    assert len(search('OtherNeedle'))==1
    request('index.pause',value=False)
    add('ack-fault','DeletedAgainNeedle');assert lib.dc_index_pending(db,16)==0
    cache.execute('BEGIN IMMEDIATE');assert lib.dc_delete_chat(db,b'ack-fault');cache.rollback()
    source.execute("CREATE TRIGGER owned_cleanup_ack_fault BEFORE DELETE ON index_deletions WHEN OLD.chat='ack-fault' BEGIN SELECT RAISE(ABORT,'owned cleanup acknowledgement failure'); END");source.commit()
    assert lib.dc_index_pending(db,16)==-1
    assert cache.execute("SELECT count(*) FROM history_fts WHERE chat='ack-fault'").fetchone()[0]==0
    assert source.execute("SELECT count(*) FROM index_deletions WHERE chat='ack-fault'").fetchone()[0]==1
    add('ack-fault','RecreatedAfterCleanupNeedle')
    source.execute('DROP TRIGGER owned_cleanup_ack_fault');source.commit()
    assert lib.dc_index_pending(db,16)==0
    assert not search('DeletedAgainNeedle') and len(search('RecreatedAfterCleanupNeedle'))==1
    print('PASS failed source acknowledgement after actual cache deletion remains retryable before new jobs',flush=True)
    source.close();cache.close();lib.dc_close(db)
    print('PASS source rollback preserves caches and cleanup intent; paused indexing still removes deleted cache',flush=True)
    (private/'history.sqlite').write_bytes(b'owned corrupt derived cache')
    db=lib.dc_open(str(private).encode());assert db
    assert not request('index.status')['available']
    assert lib.dc_delete_chat(db,b'other')
    assert request('index.status')['pendingDeletes']==1
    assert lib.dc_index_pending(db,16)==-1
    request('index.rebuild');assert lib.dc_index_pending(db,16)==0
    assert request('index.status')['pendingDeletes']==0
    assert not search('OtherNeedle') and len(search('RecreatedAfterCleanupNeedle'))==1
    lib.dc_close(db)
    print('PASS unavailable derived cache preserves source deletion intent and recovers through explicit rebuild',flush=True)
