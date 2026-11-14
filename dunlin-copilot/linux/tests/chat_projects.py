#!/usr/bin/env python3
"""Run the native chat/project driver against a disposable loopback model."""
import http.server
import hashlib
import sqlite3
import json
import pathlib
import subprocess
import sys
import tempfile
import time
import threading

class Peer(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def add_receipt(self, chat):
        work, state = self.server.work, self.server.state
        before, after = "restored original", "current revision"
        filename = "receipt-fixture.txt"
        (work / filename).write_text(after)
        stat = work.stat()
        hash_text = lambda text: hashlib.sha256(text.encode()).hexdigest()
        with sqlite3.connect(str(state / "conversations.sqlite")) as db:
            db.create_function("unixepoch", 0, lambda: int(time.time()))
            for value in (before, after):
                db.execute("INSERT INTO file_history_blobs(hash,text,bytes) VALUES(?,?,?)",
                           (hash_text(value), value, len(value)))
            db.execute("""INSERT INTO file_operations
                    (id,chat,run,call_id,proposal,grant_id,project,root,device,inode,path,operation,before_hash,after_hash,
                     existed,after_exists,args_hash,state)
                    VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                       ("f" * 64,chat,"historical-run","historical-call",0,0,"p",str(work),str(stat.st_dev),
                        str(stat.st_ino),filename,"write_file",hash_text(before),hash_text(after),1,1,"0" * 64,"applied"))
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert body["model"] == "fixture" and self.path == "/v1/chat/completions"
        assert not self.headers.get("Authorization")
        self.server.requests.append(body)
        messages = body["messages"]
        last = messages[-1]
        image = isinstance(last["content"], list)
        if image:
            parts = last["content"]
            task = next(it["text"] for it in parts if it["type"] == "text")
            url = next(it["image_url"]["url"] for it in parts if it["type"] == "image_url")
            import base64, struct
            binary = base64.b64decode(url.split(",", 1)[1])
            assert binary[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack(">II", binary[16:24]) == (4, 3)
        else:
            task = last["content"]
        self.server.seen.append((last["role"], task, body.get("tools")))
        if task == "retry-fixture" and not getattr(self.server, "failed_once", False):
            self.server.failed_once = True
            self.send_error(422, "owned fixture transport error")
            return
        message = {"role": "assistant"}
        reason = "stop"
        if image:
            assert "image-prompt" in task, task
            message["content"] = "Image received"
        elif "attachment-message" in task and "attachment-body" in task:
            message["content"] = "Attachment received"
        elif "reference-data" in task and "project-reference" in task:
            assert "source-file-content" in task
            message["content"] = "Reference received"
        elif "project-reference" in task:
            raise AssertionError("selected source missing")
        elif task == "rich-fixture":
            message["content"] = "# Result\n- **safe** with `code`\n\n| A | B |\n| --- | --- |\n| 1 | 2 |\n\n```c\n<tag>\n```"
        elif task == "html-fixture":
            message["content"] = "<article><h1>Native HTML</h1><p><b>emphasis</b></p><script>never-load</script><img alt='safe alternative' src='http://127.0.0.1:1/no' /></article>"
        elif task == "deny-write":
            with sqlite3.connect(str(self.server.state / "conversations.sqlite")) as db:
                rows = db.execute("SELECT id FROM chats WHERE workspace=?", (str(self.server.work),)).fetchall()
            assert len(rows) == 1
            self.add_receipt(rows[0][0])
            message["content"] = None
            message["tool_calls"] = [{"id": "test-denial", "type": "function",
                "function": {"name": "write_file", "arguments": json.dumps({
                    "path": "disallowed.txt", "text": "must never be written",
                    "expected_sha256": "new"})}}]
            reason = "tool_calls"
        else:
            message["content"] = ("Permission denied and preserved" if last["role"] == "tool"
                                  else "Reply " + task.split("\n", 1)[0])
        if body.get("stream"):
            delta = dict(message)
            if "tool_calls" in delta:
                delta["tool_calls"][0]["index"] = 0
            chunk = {"choices": [{"index": 0, "delta": delta, "finish_reason": reason}]}
            data = ("data: " + json.dumps(chunk) + "\n\ndata: [DONE]\n\n").encode()
            mime = "text/event-stream"
        else:
            data = json.dumps({"choices": [{"index": 0, "message": message, "finish_reason": reason}]}).encode()
            mime = "application/json"
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

with tempfile.TemporaryDirectory(prefix="dunlin-gtk-") as tmp:
    root = pathlib.Path(tmp)
    state, work = root / "state", root / "work"
    state.mkdir(mode=0o700)
    (work / "src").mkdir(parents=True)
    (work / "src" / "fixture.txt").write_text("source-file-content")
    (work / "attach.txt").write_text("attachment-body")
    server = http.server.HTTPServer(("127.0.0.1", 0), Peer)
    server.work, server.state = work, state
    server.seen = []
    server.requests = []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        subprocess.run([sys.argv[1], str(state), str(work),
                        "http://127.0.0.1:{}/v1".format(server.server_port)], timeout=90, check=True)
        arrivals = [task for role, task, _ in server.seen if role == "user"]
        expected = {"ordinary-fixture", "rich-fixture", "html-fixture", "project-fixture", "deny-write", "agent-one", "agent-two"}
        assert expected <= {item.split("\n", 1)[0] for item in arrivals}, arrivals
        assert sum("source-file-content" in t for t in arrivals) == 1
        assert any("reference-data" in t and "project-reference" in t for t in arrivals)
        assert any("collaboration-example" in json.dumps(body, ensure_ascii=False) for body in server.requests
                   if str(body["messages"][-1]["content"]).startswith("agent-two"))
        assert not next(tools for role, task, tools in server.seen if task == "ordinary-fixture")
        assert next(tools for role, task, tools in server.seen if task == "project-fixture")
        print("PASS loopback transport, project tool scope, both worker transcripts")
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
