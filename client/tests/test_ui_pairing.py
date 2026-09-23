"""Exercise GTK Enter, claim persistence, failed email update, and retry."""

import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit


def main(binary: Path) -> None:
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def reply(self, status, data):
            payload = json.dumps(data).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            requests.append(("POST", self.path, body))
            self.reply(200, {"token": "mock-device-token", "kindle_email": "old@kindle.com"})

        def do_PUT(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            requests.append(("PUT", self.path, body))
            if sum(request[0] == "PUT" for request in requests) == 1:
                self.reply(503, {"detail": "temporary email update failure"})
            else:
                self.reply(200, {"kindle_email": "reader@kindle.com"})

        def do_GET(self):
            requests.append(("GET", self.path, None))
            if self.path == "/v1/categories":
                if sum(method == "GET" and path == "/v1/categories" for method, path, _ in requests) == 1:
                    self.reply(503, {"detail": "temporary catalog failure"})
                else:
                    self.reply(200, {"categories": []})
            elif urlsplit(self.path).path == "/v1/search":
                self.reply(200, {"items": [{"id": "123", "title": "A Book", "author": "An Author", "year": 2022}], "page": 1, "has_next": True})
            else:
                self.reply(200, {"status": "pending", "expires_at": "2026-09-23T12:00:00+00:00"})

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as directory:
            environment = os.environ.copy()
            environment["BOOKRELAY_TEST_PAIR_URL"] = f"http://127.0.0.1:{server.server_port}"
            environment["BOOKRELAY_TEST_CATEGORY_RETRY"] = "1"
            result = subprocess.run(
                ["xvfb-run", "-a", "-s", "-screen 0 1264x1680x24", str(binary), directory],
                env=environment, timeout=20, capture_output=True, text=True,
            )
            if result.returncode:
                raise AssertionError(f"GTK test failed ({result.returncode}): {result.stderr}\n{result.stdout}")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    assert [request for request in requests if request[0] == "POST"] == [
        ("POST", "/v1/pair/claim", {"code": "ABCDEF12"})
    ], requests
    assert [request for request in requests if request[0] == "PUT"] == [
        ("PUT", "/v1/devices/me", {"kindle_email": "reader@kindle.com"}),
        ("PUT", "/v1/devices/me", {"kindle_email": "reader@kindle.com"}),
    ], requests
    assert len([request for request in requests if request == ("GET", "/v1/categories", None)]) == 2, requests
    searches = [parse_qs(urlsplit(path).query) for method, path, _ in requests
                if method == "GET" and urlsplit(path).path == "/v1/search"]
    assert searches == [{"q": ["test book"], "page": ["1"], "size": ["12"]}], requests


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve())
