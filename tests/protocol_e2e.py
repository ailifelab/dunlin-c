#!/usr/bin/env python3
"""Local HTTP protocol fixtures through the CLI, real tools and durable sessions.

All model output below is fixed test data. The sole approval is an exact reviewed
write_file fixture; this driver never approves a live model or connects off-host.
Reported token values come from fixtures, not a real tokenizer or billing meter.
"""
import json
import os
import pty
import select
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from socketserver import ThreadingMixIn


TEXT = "fixture tool effect: 你好"
THINKING = "Check the returned tool result."
OPAQUE = "fixture-opaque-continuation"
KEY = "fixture-only-not-a-user-credential"


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def sse(value):
    return b"event: " + value["type"].encode() + b"\ndata: " + encoded(value) + b"\n\n"


def response_payload(protocol, stage, stream):
    name = "write_file" if stage == 0 else "read_file"
    args = {"path": "observed.txt", "text": TEXT} if stage == 0 else {"path": "observed.txt"}
    call_id = "call_write" if stage == 0 else "call_read"
    if protocol == "responses":
        items = [{"type": "reasoning", "id": "reason_" + str(stage),
                  "summary": [{"type": "summary_text", "text": THINKING}], "encrypted_content": OPAQUE}]
        if stage < 2:
            items.append({"type": "function_call", "id": "item_" + str(stage),
                          "call_id": call_id, "name": name, "arguments": encoded(args).decode(), "status": "completed"})
        else:
            items.append({"type": "message", "id": "message_final", "role": "assistant", "status": "completed",
                          "content": [{"type": "output_text", "text": TEXT, "annotations": []}]})
        final = {"id": "resp_" + str(stage), "status": "completed", "output": items,
                 "usage": {"input_tokens": 20, "output_tokens": 12, "total_tokens": 32,
                           "input_tokens_details": {"cached_tokens": 3},
                           "output_tokens_details": {"reasoning_tokens": 2}}}
        if not stream:
            return encoded(final)
        events = [{"type": "response.created", "response": {"id": final["id"], "status": "in_progress", "output": []}}]
        for index, item in enumerate(items):
            initial = dict(item)
            if item["type"] == "reasoning":
                initial["summary"] = []
                initial.pop("encrypted_content", None)
            elif item["type"] == "function_call":
                initial.update(arguments="", status="in_progress")
            else:
                initial.update(content=[], status="in_progress")
            events.append({"type": "response.output_item.added", "output_index": index, "item": initial})
            common = {"item_id": item["id"], "output_index": index}
            if item["type"] == "reasoning":
                events.append(dict(common, type="response.reasoning_summary_part.added", summary_index=0,
                                   part={"type": "summary_text", "text": ""}))
                events.append(dict(common, type="response.reasoning_summary_text.delta", summary_index=0, delta=THINKING))
                events.append(dict(common, type="response.reasoning_summary_text.done", summary_index=0, text=THINKING))
            elif item["type"] == "function_call":
                argument = item["arguments"]
                split = len(argument) // 2
                for part in (argument[:split], argument[split:]):
                    events.append(dict(common, type="response.function_call_arguments.delta", delta=part))
                events.append(dict(common, type="response.function_call_arguments.done", arguments=argument))
            else:
                events.append(dict(common, type="response.content_part.added", content_index=0,
                                   part={"type": "output_text", "text": "", "annotations": []}))
                for part in (TEXT[:8], TEXT[8:]):
                    events.append(dict(common, type="response.output_text.delta", content_index=0, delta=part))
                events.append(dict(common, type="response.output_text.done", content_index=0, text=TEXT))
                events.append(dict(common, type="response.content_part.done", content_index=0, part=item["content"][0]))
            events.append({"type": "response.output_item.done", "output_index": index, "item": item})
        events.append({"type": "response.completed", "response": final})
        return b": fixture heartbeat\r\n\r\n" + b"".join(sse(event) for event in events)
    if protocol == "anthropic":
        blocks = [{"type": "thinking", "thinking": THINKING, "signature": OPAQUE}]
        if stage < 2:
            blocks.append({"type": "tool_use", "id": call_id, "name": name, "input": args})
        else:
            blocks.append({"type": "text", "text": TEXT})
        usage = {"input_tokens": 20, "output_tokens": 12, "cache_read_input_tokens": 3, "cache_creation_input_tokens": 0}
        final = {"id": "msg_" + str(stage), "type": "message", "role": "assistant", "content": blocks,
                 "stop_reason": "tool_use" if stage < 2 else "end_turn", "usage": usage}
        if not stream:
            return encoded(final)
        initial = dict(final, content=[], stop_reason=None, usage=dict(usage, output_tokens=0))
        events = [{"type": "ping"}, {"type": "message_start", "message": initial}]
        for index, block in enumerate(blocks):
            if block["type"] == "thinking":
                initial_block = {"type": "thinking", "thinking": "", "signature": ""}
                deltas = [{"type": "thinking_delta", "thinking": THINKING}, {"type": "signature_delta", "signature": OPAQUE}]
            elif block["type"] == "tool_use":
                initial_block = dict(block, input={})
                argument = encoded(args).decode()
                split = len(argument) // 2
                deltas = [{"type": "input_json_delta", "partial_json": part} for part in (argument[:split], argument[split:])]
            else:
                initial_block = {"type": "text", "text": ""}
                deltas = [{"type": "text_delta", "text": part} for part in (TEXT[:8], TEXT[8:])]
            events.append({"type": "content_block_start", "index": index, "content_block": initial_block})
            events.extend({"type": "content_block_delta", "index": index, "delta": delta} for delta in deltas)
            events.append({"type": "content_block_stop", "index": index})
        events.extend([{"type": "message_delta", "delta": {"stop_reason": final["stop_reason"]}, "usage": {"output_tokens": 12}},
                       {"type": "message_stop"}])
        return b"".join(sse(event) for event in events)
    if protocol == "ollama":
        message = {"role": "assistant", "content": "" if stage < 2 else TEXT, "thinking": THINKING}
        if stage < 2:
            message["tool_calls"] = [{"function": {"index": 0, "name": name, "arguments": args}}]
        final = {"model": "fixture", "message": message, "done": True, "done_reason": "stop",
                 "prompt_eval_count": 20, "prompt_eval_cached_count": 3, "eval_count": 12}
        if not stream:
            return encoded(final)
        chunks = [{"message": {"role": "assistant", "thinking": THINKING, "content": ""}, "done": False}]
        if stage < 2:
            chunks.append({"message": {"role": "assistant", "content": "", "tool_calls": message["tool_calls"]}, "done": False})
        else:
            chunks.extend({"message": {"role": "assistant", "content": part}, "done": False} for part in (TEXT[:8], TEXT[8:]))
        chunks.append(dict(final, message={"role": "assistant", "content": ""}))
        return b"\r\n".join(encoded(chunk) for chunk in chunks)  # final line intentionally lacks LF
    if protocol == "gemini":
        parts = [{"thought": True, "text": THINKING, "thoughtSignature": OPAQUE}]
        if stage < 2:
            parts.append({"functionCall": {"name": name, "args": args}})
        else:
            parts.append({"text": TEXT})
        final = {"candidates": [{"content": {"role": "model", "parts": parts}, "finishReason": "STOP"}],
                 "usageMetadata": {"promptTokenCount": 20, "candidatesTokenCount": 12,
                                   "totalTokenCount": 32, "cachedContentTokenCount": 3}}
        if not stream:
            return encoded(final)
        return b"data: " + encoded(final) + b"\n\n"
    raise AssertionError(protocol)


class Server(ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.protocol = None
        self.stream = False
        self.stage = 0
        self.requests = []
        self.failures = []
        self.response_bytes = []

    def healthy(self):
        assert not self.failures, self.failures


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *args):
        pass

    def do_POST(self):
        server = self.server
        try:
            body = self.rfile.read(int(self.headers["Content-Length"]))
            req = json.loads(body.decode("utf-8"))
            protocol, stage = server.protocol, server.stage
            assert stage < 3, ("unexpected request after completion", req)
            assert self.path == {"responses": "/v1/responses", "anthropic": "/v1/messages", "ollama": "/api/chat",
                                 "gemini": "/v1/models/fixture:streamGenerateContent?alt=sse" if server.stream else
                                           "/v1/models/fixture:generateContent"}[protocol]
            if protocol == "gemini":
                assert self.headers.get("Authorization") is None
            else:
                assert self.headers.get("Authorization") == "Bearer " + KEY
            assert self.headers.get("Content-Type") == "application/json"
            assert self.headers.get("Accept") == ("application/x-ndjson" if server.stream and protocol == "ollama" else
                                                   "text/event-stream" if server.stream else "application/json")
            if protocol == "gemini":
                assert self.headers.get("x-goog-api-key") == KEY
            if protocol != "gemini":
                assert req["stream"] is server.stream
            assert "_dunlin_provider" not in body.decode(), "private normalized envelope leaked to provider"
            if protocol == "responses":
                assert req["max_output_tokens"] == 128 and "messages" not in req
                assert req["store"] is False
                assert {tool["name"] for tool in req["tools"]} == {"write_file", "read_file"}
                results = [item for item in req["input"] if item.get("type") == "function_call_output"]
                if stage:
                    assert results[-1]["call_id"] == ("call_write" if stage == 1 else "call_read")
                    assert results[-1]["output"] == ("written" if stage == 1 else TEXT)
                    assert any(item.get("encrypted_content") == OPAQUE for item in req["input"])
            elif protocol == "anthropic":
                assert self.headers.get("anthropic-version") == "2026-10-21"
                assert req["max_tokens"] == 128
                assert {tool["name"] for tool in req["tools"]} == {"write_file", "read_file"}
                results = [part for item in req["messages"] for part in item["content"] if part.get("type") == "tool_result"]
                if stage:
                    assert results[-1]["tool_use_id"] == ("call_write" if stage == 1 else "call_read")
                    assert results[-1]["content"] == ("written" if stage == 1 else TEXT)
                    assert any(part.get("signature") == OPAQUE for item in req["messages"] for part in item["content"])
            elif protocol == "ollama":
                assert req["options"]["num_predict"] == 128
                assert {tool["function"]["name"] for tool in req["tools"]} == {"write_file", "read_file"}
                results = [item for item in req["messages"] if item.get("role") == "tool"]
                if stage:
                    assert results[-1]["tool_name"] == ("write_file" if stage == 1 else "read_file")
                    assert results[-1]["content"] == ("written" if stage == 1 else TEXT)
                    assert any(item.get("thinking") == THINKING for item in req["messages"])
            else:
                assert req["generationConfig"]["maxOutputTokens"] == 128
                declarations = req["tools"][0]["functionDeclarations"]
                assert {tool["name"] for tool in declarations} == {"write_file", "read_file"}
                results = [part["functionResponse"] for item in req["contents"] for part in item["parts"]
                           if "functionResponse" in part]
                if stage:
                    assert results[-1]["name"] == ("write_file" if stage == 1 else "read_file")
                    assert results[-1]["response"]["output"] == ("written" if stage == 1 else TEXT)
                    assert any("thoughtSignature" in part for item in req["contents"] for part in item["parts"])
            server.requests.append((len(body), req))
            response = response_payload(protocol, stage, server.stream)
            server.response_bytes.append(len(response))
            server.stage += 1
            self.send_response(200)
            self.send_header("Content-Type", "application/x-ndjson" if protocol == "ollama" and server.stream else
                             "text/event-stream" if server.stream else "application/json")
            self.end_headers()
            for index in range(0, len(response), 7):
                self.wfile.write(response[index:index + 7])
                self.wfile.flush()
        except Exception as error:
            server.failures.append(repr(error))
            try:
                self.send_response(500)
                self.end_headers()
            except OSError:
                pass


def execute(binary, root, config, env, session, approval=False, resume=False):
    cmd = [binary, "--config", str(config), "--permissions", "strict", "--events", "--session", session]
    # CLI run is deliberately noninteractive. Resume with a new prompt supports
    # exact TTY approvals even for a new isolated session.
    cmd += ["resume"] if resume else ["resume" if approval else "run", "Save the fixture text, read it back, and report the observed value."]
    if not approval:
        process = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        return process.returncode, process.stdout, process.stderr
    master, slave = pty.openpty()
    process = subprocess.Popen(cmd, env=env, stdin=slave, stdout=subprocess.PIPE, stderr=slave)
    os.close(slave)
    terminal = bytearray()
    approved = False
    deadline = time.monotonic() + 15
    try:
        while process.poll() is None:
            assert time.monotonic() < deadline, ("approval fixture timeout", terminal)
            if select.select([master], [], [], 0.05)[0]:
                try:
                    chunk = os.read(master, 8192)
                except OSError:
                    break
                terminal.extend(chunk)
                if b"Type yes to allow once:" in terminal and not approved:
                    text = terminal.decode("utf-8")
                    assert "Approval required for tool write_file" in text
                    json_lines = [line.strip() for line in text.splitlines() if line.strip().startswith("{")]
                    assert len(json_lines) == 1
                    assert json.loads(json_lines[0]) == {"path": "observed.txt", "text": TEXT}
                    # This exact fixed fixture is the only approval this test supplies.
                    os.write(master, b"yes\n")
                    approved = True
        output, _ = process.communicate(timeout=3)
        assert approved, (process.returncode, output, terminal)
        return process.returncode, output, bytes(terminal)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)


def run_case(binary, server, protocol, stream):
    server.protocol, server.stream, server.stage = protocol, stream, 0
    server.requests, server.response_bytes, server.failures = [], [], []
    with tempfile.TemporaryDirectory(prefix="dunlin-protocol-e2e-") as directory:
        root = Path(directory)
        (root / "work").mkdir()
        (root / "home").mkdir()
        config = root / "profile.json"
        config.write_text(json.dumps({"base_url": "http://127.0.0.1:{}/{}".format(server.server_port, "api" if protocol == "ollama" else "v1"),
                                      "protocol": protocol, "model": "fixture", "stream": stream,
                                      "key_env": "DUNLIN_PROTOCOL_TEST_KEY", "max_tokens": 128,
                                      "workspace": "work", "session_dir": "sessions", "max_rounds": 5,
                                      "max_context_bytes": 32768, "max_message_bytes": 8192,
                                      "task_timeout_ms": 10000, "timeout_ms": 5000,
                                      "enabled_tools": ["write_file", "read_file"]}))
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(root / "home"),
               "NO_PROXY": "*", "no_proxy": "*", "LC_ALL": "C.UTF-8", "DUNLIN_PROTOCOL_TEST_KEY": KEY}
        code, output, terminal = execute(binary, root, config, env, "protocol", approval=True)
        server.healthy()
        assert code == 0, (protocol, stream, code, output, terminal)
        assert KEY.encode() not in output + terminal and OPAQUE.encode() not in output + terminal
        rows = [json.loads(line) for line in output.splitlines()]
        assert rows[-1]["status"] == "completed" and rows[-1]["text"] == TEXT, rows
        assert (root / "work" / "observed.txt").read_text() == TEXT
        assert server.stage == 3
        starts = [row["data"] for row in rows if row.get("event") == "tool.start"]
        results = [row["data"] for row in rows if row.get("event") == "tool.result"]
        assert len(starts) == 2 and starts == results and len(set(starts)) == 2, rows
        assert any(row.get("event") in ("reasoning.delta", "reasoning.summary") and row["data"] == THINKING for row in rows)
        checkpoint = json.loads((root / "sessions" / "protocol.json").read_text())
        assistants = [message for message in checkpoint["messages"] if message["role"] == "assistant"]
        assert len(assistants) == 3 and all(message["_dunlin_provider"]["protocol"] == protocol for message in assistants)
        journal = (root / "sessions" / "protocol.json").read_text()
        assert (THINKING if protocol == "ollama" else OPAQUE) in journal
        before = len(server.requests)
        code, resumed, error = execute(binary, root, config, env, "protocol", resume=True)
        server.healthy()
        assert code == 0 and json.loads(resumed.splitlines()[-1])["text"] == TEXT, (resumed, error)
        assert len(server.requests) == before, "completed resume must not request a model or replay tools"
        metrics = [json.loads(row["data"]) for row in rows if row.get("event") == "model.metrics"]
        assert len(metrics) == 3, rows
        for index, metric in enumerate(metrics):
            assert metric["request_bytes"] == server.requests[index][0], metric
            assert metric["response_bytes"] == server.response_bytes[index], metric
            assert metric["response_bytes_known"] is True, metric
            assert metric["output_tokens"] == 12, metric
        print(json.dumps({"source": "deterministic local HTTP fixtures; tokens are fixture values", "protocol": protocol,
                          "stream": stream, "requests": len(server.requests), "tool_effect": "written and read back",
                          "request_bytes": sum(item[0] for item in server.requests),
                          "response_bytes": sum(server.response_bytes), "usage": metrics}, ensure_ascii=False))


def main():
    binary = os.path.abspath(sys.argv[1])
    server = Server()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        for protocol in ("responses", "anthropic", "ollama", "gemini"):
            for stream in (False, True):
                run_case(binary, server, protocol, stream)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
    print("PASS native HTTP headers/framing, tool IDs, reviewed fixture write/read effects, opaque durable continuation, no-replay resume and measured wire bytes")


if __name__ == "__main__":
    main()
