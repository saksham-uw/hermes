#!/usr/bin/env python3
"""Local Whisper STT for Hermes. Loads once, listens on 127.0.0.1:8765."""

from __future__ import annotations

import os
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from faster_whisper import WhisperModel

HOST = os.environ.get("HERMES_STT_HOST", "127.0.0.1")
PORT = int(os.environ.get("HERMES_STT_PORT", "8765"))
MODEL_NAME = os.environ.get("HERMES_STT_MODEL", "tiny.en")

print(f"[stt] loading {MODEL_NAME} (first run downloads the model)...", flush=True)
model = WhisperModel(MODEL_NAME, device="cpu", compute_type="int8")
print(f"[stt] ready on http://{HOST}:{PORT}", flush=True)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args) -> None:
        print("[stt] " + (fmt % args), flush=True)

    def do_GET(self) -> None:
        if self.path.rstrip("/") == "/health":
            body = b"ok"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_error(404)

    def do_POST(self) -> None:
        if self.path.rstrip("/") != "/transcribe":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length", "0"))
        data = self.rfile.read(n)
        if len(data) < 44:
            self.send_error(400, "empty audio")
            return
        fd, path = tempfile.mkstemp(suffix=".wav")
        try:
            os.write(fd, data)
            os.close(fd)
            segments, _info = model.transcribe(
                path, language="en", vad_filter=True, beam_size=1
            )
            text = " ".join(seg.text.strip() for seg in segments).strip()
        finally:
            try:
                os.unlink(path)
            except OSError:
                pass
        body = text.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    httpd = ThreadingHTTPServer((HOST, PORT), Handler)
    httpd.serve_forever()
