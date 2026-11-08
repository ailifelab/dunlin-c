"""Out-of-band corruption/setup of OWNED desktop test stores only.

The new C desktop connection registers a source-writer capability. Tests that
deliberately inject malformed/pending canonical state need their own explicit
capability on that Python fixture connection. This neither changes a C library's
connection nor authorizes tools/model commands. Never import in production.
"""
import sqlite3

def connect(*args, **kwargs):
    db=sqlite3.connect(*args,**kwargs)
    db.create_function('dunlin_workspace_writer_v1',0,lambda:1)
    return db
