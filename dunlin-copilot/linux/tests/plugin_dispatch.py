#!/usr/bin/env python3
"""Use a pinned original DSH Project Manager package in a disposable store."""
import http.server
import json
import sqlite3
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading

class Model(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert self.path == "/v1/chat/completions" and body["model"] == "fixture"
        assert not self.headers.get("Authorization")
        task = body["messages"][-1]["content"]
        if "sdk-prompt" in task:
            answer = "Worker result"
        elif "helper-prompt" in task:
            answer = "A short title"
            assert not body.get("tools") and len(body["messages"]) == 2
            assert body["messages"][0]["role"] == "system"
        else:
            raise AssertionError(task)
        self.server.calls.append(answer)
        if body.get("stream"):
            payload = {"choices": [{"index": 0, "delta": {"content": answer}, "finish_reason": "stop"}]}
            wire = ("data: " + json.dumps(payload) + "\n\ndata: [DONE]\n\n").encode()
            mime = "text/event-stream"
        else:
            wire = json.dumps({"choices": [{"index": 0, "message": {"role": "assistant", "content": answer}, "finish_reason":"stop"}]}).encode()
            mime = "application/json"
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length",str(len(wire)))
        self.end_headers()
        self.wfile.write(wire)

with tempfile.TemporaryDirectory(prefix="dunlin-sdk-") as tmp:
    directory = pathlib.Path(tmp)
    source = pathlib.Path(sys.argv[4])
    package = directory/"original"
    shutil.copytree(str(source),str(package))
    manifest_path = package/"dunlin.json"
    manifest = json.loads(manifest_path.read_text())
    assert manifest["packageId"] == "dsh-project-manager" and manifest["version"] == "0.57.0"
    manifest.update(version="0.57.0+dunlin.3",upstreamVersion="0.57.0",adapterRevision=3,
                    client={"format":"dsh-lazy-cjs@1","entry":"lib/client.js","slots":["dunlin.workbench"]})
    manifest_path.write_text(json.dumps(manifest))
    data,work = directory/"data", directory/"workspace"
    data.mkdir(mode=0o700);work.mkdir(mode=0o700)
    server=http.server.HTTPServer(("127.0.0.1",0),Model)
    server.calls=[]
    threading.Thread(target=server.serve_forever,daemon=True).start()
    try:
        subprocess.run([sys.argv[1],str(data),str(work),"http://127.0.0.1:{}/v1".format(server.server_port),
                        sys.argv[2],sys.argv[3],str(package)],check=True,timeout=80)
        assert server.calls == ["Worker result","A short title"],server.calls
        with sqlite3.connect(str(data / "conversations.sqlite")) as db:
            assert db.execute("SELECT state,text FROM model_helper_jobs").fetchall() == [("reported","A short title")]
            assert db.execute("SELECT state FROM agent_jobs WHERE id='manual'").fetchone() == ("queued",)
            assert db.execute("SELECT j.state,v.output FROM agent_jobs j JOIN agent_job_views v ON v.job=j.id WHERE j.id<>'manual'").fetchall() == [("succeeded","Worker result")]

        print("PASS independent HTTP peer received both original plugin requests; text helper received no tools")
    finally:
        server.shutdown();server.server_close()
