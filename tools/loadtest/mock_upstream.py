#!/usr/bin/env python3
"""Mock OpenAI-compatible upstream for NixRoute stress testing.

Behaviours are selected by the *model* id in the chat request:

  mockok      small OK (stream or JSON)
  mockecho    echoes back your exact last user message
  mock429 / mock500 / mock503 / mock400 / mock401
  mockhang    accepts the body then never replies (device must time out)
  mockreset   accepts body then RSTs the connection
  mockmid     streams some SSE then abruptly dies mid-stream
  mockbig     non-streaming ~200 KB JSON
  mockhuge    streaming SSE ~300 KB total, many chunks
  mockchunks  chunked response with one very large chunk (> buffer size)
  mockblob    returns the request body size + hash in a tiny reply

GET /v1/models returns all of the above ids.
"""
import argparse, json, time, hashlib, socket, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODELS = ["mockok", "mockecho", "mock429", "mock500", "mock503", "mock400",
          "mock401", "mockhang", "mockreset", "mockmid", "mockbig",
          "mockhuge", "mockchunks", "mockblob"]

COUNTER = {"requests": 0, "models": 0, "bytes_out": 0, "resets": 0}
_lock = threading.Lock()

def log(*a):
    print("[mock]", *a, flush=True)


def read_body(self):
    # Decode Content-Length or chunked request bodies (device uploads chunked).
    if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
        chunks = []
        while True:
            line = self.rfile.readline().strip()
            if not line:
                break
            try:
                size = int(line.split(b";")[0], 16)
            except ValueError:
                break
            if size == 0:
                self.rfile.readline()  # trailing CRLF
                break
            chunks.append(self.rfile.read(size))
            self.rfile.read(2)  # CRLF
        return b"".join(chunks)
    length = int(self.headers.get("Content-Length") or 0)
    if length <= 0:
        return b""
    return self.rfile.read(length)


def abort(sock):
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                        struct_pack_linger(0, 0))
    except Exception:
        pass
    try:
        sock.close()
    except Exception:
        pass


def struct_pack_linger(on, secs):
    import struct
    return struct.pack("ii", on, secs)


def chunk_line(sock, data):
    sock.write(("%x\r\n" % len(data)).encode() + data + b"\r\n")
    try:
        sock.flush()
    except Exception:
        pass


def usage_json(pt=12, ct=8):
    return json.dumps({
        "prompt_tokens": pt, "completion_tokens": ct, "total_tokens": pt + ct,
        "input_tokens": pt, "output_tokens": ct,
    })


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _send_sse_head(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()

    def _sse_event(self, sock, obj):
        chunk_line(sock, b"data: " + json.dumps(obj).encode() + b"\n\n")

    def do_GET(self):
        if self.path.startswith("/v1/models"):
            with _lock:
                COUNTER["models"] += 1
            self._send(200, {"object": "list", "data": [
                {"id": m, "object": "model", "owned_by": "mock"} for m in MODELS]})
        else:
            self._send(200, {"status": "mock-up ok"})

    def do_POST(self):
        body = read_body(self)
        try:
            req = json.loads(body or b"{}")
        except Exception:
            req = {}
        model = (req.get("model") or "mockok").split("/")[-1]
        stream = bool(req.get("stream"))
        msgs = req.get("messages") or []
        user_content = ""
        for m in reversed(msgs):
            if m.get("role") == "user":
                user_content = m.get("content") or ""
                break
        with _lock:
            COUNTER["requests"] += 1
        total = len(body)

        def finish(cb):
            self.close_connection = True
            cb()

        if model in ("mock429", "mock500", "mock503", "mock400", "mock401"):
            code = int(model[4:])
            self._send(code, {"error": {"message": "mock " + str(code),
                                        "type": "mock", "code": code}})
            return
        if model == "mockhang":
            # Read done; never answer. Keep socket open until peer gives up.
            try:
                time.sleep(90)
            except Exception:
                pass
            return
        if model == "mockreset":
            with _lock:
                COUNTER["resets"] += 1
            abort(self.connection)
            return
        if model == "mockmid":
            self._send_sse_head()
            try:
                for i in range(6):
                    self._sse_event(self.wfile, {"id": "m", "object": "chat.completion.chunk",
                                                 "choices": [{"delta": {"content": "partial-%d " % i},
                                                              "index": 0}]})
                    self.wfile.flush()
                    time.sleep(0.05)
            except Exception:
                pass
            with _lock:
                COUNTER["resets"] += 1
            abort(self.connection)
            return
        if model == "mockbig":
            payload = json.dumps({"choices": [{"message": {"role": "assistant",
                                    "content": ("B" * (180 * 1024)) + "ENDMARKER"}}],
                                  "usage": json.loads(usage_json(20, 5000))})
            body_b = payload.encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body_b)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body_b)
            return
        if model == "mockhuge":
            self._send_sse_head()
            text = ("The quick brown fox jumps over the lazy dog. " * 400)  # ~13k per block
            sent = 0
            try:
                for block in range(0, 24):
                    piece = ("%s chunk-%d " % (text, block))[:8192]
                    self._sse_event(self.wfile, {"id": "h", "object": "chat.completion.chunk",
                                                 "choices": [{"delta": {"content": piece}, "index": 0}]})
                    self.wfile.flush()
                    sent += len(piece) + 120
                    time.sleep(0.01)
                self._sse_event(self.wfile, {"id": "h", "object": "chat.completion.chunk",
                                             "choices": [{"delta": {}, "finish_reason": "stop", "index": 0}],
                                             "usage": json.loads(usage_json(30, sent // 4))})
                chunk_line(self.wfile, b"data: [DONE]\n\n")
                chunk_line(self.wfile, b"")
            except Exception:
                pass
            self.close_connection = True
            return
        if model == "mockchunks":
            self._send_sse_head()
            try:
                big = b"data: " + b"X" * 12000 + b"\n\n"
                chunk_line(self.wfile, big)  # single chunk far above 1 KB buffer
                chunk_line(self.wfile, b"data: [DONE]\n\n")
                chunk_line(self.wfile, b"")
            except Exception:
                pass
            self.close_connection = True
            return
        if model == "mockblob":
            h = hashlib.sha256(body).hexdigest()[:16]
            self._send(200, {"choices": [{"message": {"role": "assistant",
                                      "content": "body=%d hash=%s" % (total, h)}}],
                             "usage": json.loads(usage_json(0, 0))})
            return
        # mockok / mockecho / default
        if model == "mockecho":
            content = user_content
        else:
            content = "mock-ok reply to: " + (user_content[:40] or "(empty)")
        if stream:
            self._send_sse_head()
            try:
                for piece in (content[i:i + 24] for i in range(0, len(content) + 1, 24)):
                    self._sse_event(self.wfile, {"id": "o", "object": "chat.completion.chunk",
                                                 "choices": [{"delta": {"content": piece}, "index": 0}]})
                    self.wfile.flush()
                    time.sleep(0.002)
                self._sse_event(self.wfile, {"id": "o", "object": "chat.completion.chunk",
                                             "choices": [{"delta": {}, "finish_reason": "stop", "index": 0}],
                                             "usage": json.loads(usage_json(9, 5))})
                chunk_line(self.wfile, b"data: [DONE]\n\n")
                chunk_line(self.wfile, b"")
            except Exception:
                pass
            self.close_connection = True
            return
        self._send(200, {"id": "mock", "object": "chat.completion", "model": model,
                         "choices": [{"index": 0, "message": {"role": "assistant", "content": content},
                                      "finish_reason": "stop"}],
                         "usage": json.loads(usage_json(9, 5))})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--host", default="0.0.0.0")
    a = ap.parse_args()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    print("mock upstream on %s:%d models=%s" % (a.host, a.port, MODELS), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
