import json
import subprocess
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class ApiHttpTests(unittest.TestCase):
    def test_pair_claim_accepts_spaced_relay_json(self):
        received = []

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                received.append((self.command, self.path, json.loads(self.rfile.read(int(self.headers["Content-Length"])))))
                self.reply({"token": "mock-device-token", "kindle_email": "reader@kindle.com"})

            def reply(self, data):
                payload = json.dumps(data).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *_args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "glib-2.0", "libcurl"], text=True).split()
            with tempfile.TemporaryDirectory() as temporary:
                binary = Path(temporary) / "api-http-smoke"
                subprocess.run(["cc", "-std=c11", str(ROOT / "client/tests/api_http_smoke.c"),
                                str(ROOT / "client/src/api.c"), "-o", str(binary), *flags],
                               check=True, capture_output=True, text=True)
                base = f"http://127.0.0.1:{server.server_port}/"
                subprocess.run([str(binary), base, "pair"], check=True, capture_output=True, text=True, timeout=10)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
        self.assertEqual(received, [
            ("POST", "/v1/pair/claim", {"code": "abcdef12"}),
        ])

    def test_pair_success_without_token_reports_protocol_error(self):
        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                payload = b'{"status": "ok"}'
                self.send_response(200)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *_args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "glib-2.0", "libcurl"], text=True).split()
            with tempfile.TemporaryDirectory() as temporary:
                binary = Path(temporary) / "api-http-smoke"
                subprocess.run(["cc", "-std=c11", str(ROOT / "client/tests/api_http_smoke.c"),
                                str(ROOT / "client/src/api.c"), "-o", str(binary), *flags],
                               check=True, capture_output=True, text=True)
                subprocess.run([str(binary), f"http://127.0.0.1:{server.server_port}", "invalid-response"],
                               check=True, capture_output=True, text=True, timeout=10)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_update_email_sends_put_with_json_and_token(self):
        received = {}

        class Handler(BaseHTTPRequestHandler):
            def do_PUT(self):
                received["path"] = self.path
                received["authorization"] = self.headers.get("Authorization")
                received["content_type"] = self.headers.get("Content-Type")
                received["body"] = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                payload = b'{"kindle_email":"reader@kindle.com"}'
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *_args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            flags = subprocess.check_output(
                ["pkg-config", "--cflags", "--libs", "glib-2.0", "libcurl"], text=True
            ).split()
            with tempfile.TemporaryDirectory() as temporary:
                binary = Path(temporary) / "api-http-smoke"
                subprocess.run(
                    ["cc", "-std=c11", str(ROOT / "client/tests/api_http_smoke.c"),
                     str(ROOT / "client/src/api.c"), "-o", str(binary), *flags],
                    check=True, capture_output=True, text=True,
                )
                subprocess.run(
                    [str(binary), f"http://127.0.0.1:{server.server_port}"],
                    check=True, capture_output=True, text=True, timeout=10,
                )
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

        self.assertEqual(received, {
            "path": "/v1/devices/me",
            "authorization": "Bearer test-device-token",
            "content_type": "application/json",
            "body": {"kindle_email": "reader@kindle.com"},
        })


if __name__ == "__main__":
    unittest.main()
