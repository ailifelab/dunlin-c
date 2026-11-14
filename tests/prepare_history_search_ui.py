#!/usr/bin/env python3
"""Exercise history search on actual local chat turns; optionally retain isolated UI data."""
import ctypes as C
import http.server
import json
from pathlib import Path
import sys
import tempfile
import threading

lib = C.CDLL(sys.argv[1])
P, S = C.c_void_p, C.c_char_p
for name, args, result in (
    ("dc_open", [S], P), ("dc_close", [P], None), ("dc_free", [P], None),
    ("dc_add_chat", [P, S, S, S], C.c_int), ("dc_workspace_request", [P, S], P),
    ("dc_run", [P, S, S, S, P], P), ("dc_messages", [P, S], P),
    ("dc_search_filtered", [P, S, S, C.c_int64], P), ("dc_index_pending", [P, C.c_uint], C.c_int),
):
    f = getattr(lib, name)
    f.argtypes, f.restype = args, result

def value(ptr):
    assert ptr
    try:
        return json.loads(C.string_at(ptr))
    finally:
        lib.dc_free(ptr)

def encode(data):
    return json.dumps(data, ensure_ascii=False).encode()

requests = []
class Model(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        requests.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
        data = encode({"choices": [{"index": 0, "message": {"role": "assistant", "content": "收到。"},
                                    "finish_reason": "stop"}]})
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

def verify(root, model):
    state, project = root / "data", root / "work"
    state.mkdir(mode=0o700)
    project.mkdir()
    ordinary = root / "ordinary"
    ordinary.mkdir()
    db = lib.dc_open(str(state).encode())
    assert db

    def request(op, **args):
        return value(lib.dc_workspace_request(db, encode(dict(op=op, **args))))

    config = encode(dict(base_url=f"http://127.0.0.1:{model.server_port}/v1", model="local-fixture",
                         protocol="chat", stream=False, desktop_tools="noTools", max_tokens=128))
    decision = "不要自动升级 Node。先让我确认升级风险。"
    correction = "补充一下：测试环境可以自动升级，正式环境仍然需要我确认。"
    query = "找之前说不要自动升级 Node 的那段".encode()
    try:
        assert request("projects.create", id="history-project", name="兼容性验收", path=str(project),
                       instructions="", model="", policy="noTools", context=[])["ok"]
        a, b = b"upgrade-policy", b"test-schedule"
        assert lib.dc_add_chat(db, a, "兼容环境升级策略".encode(), str(project).encode())
        assert lib.dc_add_chat(db, b, "测试环境安排".encode(), str(ordinary).encode())
        for chat, text in [(a, decision), (a, correction), (b, "Node 的升级先在测试环境验证，再决定正式环境怎么做。")]:
            outcome = value(lib.dc_run(db, chat, config, text.encode(), None))
            assert outcome["status"] == "completed", outcome
        original = value(lib.dc_messages(db, a))
        assert lib.dc_index_pending(db, 100) == 0
        hits = value(lib.dc_search_filtered(db, query, None, 0))
        assert {row["chat"] for row in hits} == {a.decode(), b.decode()}
        hit = next(row for row in hits if row["text"] == decision)
        assert hit["role"] == "user" and hit["time"] > 0 and "node" in hit["matchSnippet"].lower()
        filtered = value(lib.dc_search_filtered(db, query, b"history-project", 0))
        assert {row["chat"] for row in filtered} == {a.decode()}
        key = {k: hit[k] for k in ["chat", "sequence", "text", "time", "role"]}
        source = request("index.source", **key)
        assert source["ok"] and [line["text"] for line in source["data"]] == [decision, "收到。", correction], source
        assert request("index.source", **dict(key, time=hit["time"] + 1))["error"] == "source_changed"
        assert value(lib.dc_messages(db, a)) == original
        assert len(requests) == 3
        print(f"PASS local chat turns, original negation and follow-up, actual match terms, scope, timestamp, stale source; {len(hits)} hits")
    finally:
        lib.dc_close(db)

with tempfile.TemporaryDirectory(prefix="dunlin-search-") as tmp:
    root = Path(tmp)
    if len(sys.argv) > 2:
        root = Path(sys.argv[2]).resolve()
        root.mkdir(mode=0o700, parents=True, exist_ok=False)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Model)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        verify(root, server)
    finally:
        server.shutdown()
        server.server_close()
