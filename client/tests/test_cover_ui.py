"""Exercise a relay-served JPEG through the native GTK card and capture it."""

import os
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
EXPECTED_PATH = "/v1/books/451198/cover?path=%2Fi%2F98%2F451198%2Fcover.jpg"


def main(binary: str, output: str) -> None:
    jpeg = next((ROOT / "client/share/covers").glob("*.jpg")).read_bytes()
    requests = []
    active = 0
    peak = 0
    cover_attempts = 0
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            nonlocal active, peak, cover_attempts
            stress_request = self.path != EXPECTED_PATH
            with lock:
                requests.append((self.path, self.headers.get("Authorization")))
                if stress_request:
                    active += 1
                    peak = max(peak, active)
            if self.path != EXPECTED_PATH and not any(
                self.path == f"/v1/books/{i}/cover?path=%2Fi%2F98%2F{i}%2Fcover.jpg"
                for i in list(range(1000, 1012)) + list(range(1100, 1196))
            ):
                self.send_error(404)
            else:
                time.sleep(0.35 if stress_request else 0.08)
                # Measure the slow upstream phase. Once it finishes, the
                # client can release its worker before this handler returns.
                with lock:
                    if stress_request:
                        active -= 1
                if self.path == EXPECTED_PATH:
                    cover_attempts += 1
                broken = self.path == EXPECTED_PATH and cover_attempts == 1
                payload = b"not an image" if broken else jpeg
                self.send_response(200)
                self.send_header("Content-Type", "image/jpeg")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as data_home:
            env = dict(os.environ, BOOKRELAY_TEST_DPI="300", XDG_DATA_HOME=data_home,
                       BOOKRELAY_TEST_CORRUPT_COVER="1",
                       BOOKRELAY_TEST_COVER_RELAY=f"http://127.0.0.1:{server.server_port}")
            subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1264x1680x24", binary, output],
                           check=True, env=env, timeout=40)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    assert len(requests) < 13 + 96, len(requests)
    assert requests[:2] == [(EXPECTED_PATH, "Bearer smoke-test-token")] * 2, requests[:2]
    assert all(auth == "Bearer smoke-test-token" for _, auth in requests), requests
    assert peak <= 3, peak
    assert (Path(output) / "book-list-with-cover.png").is_file()
    assert (Path(output) / "book-list-after-rapid-navigation.png").is_file()
    for book_id in range(1184, 1196):
        expected = f"/v1/books/{book_id}/cover?path=%2Fi%2F98%2F{book_id}%2Fcover.jpg"
        assert any(path == expected for path, _ in requests), expected


if __name__ == "__main__":
    main(*sys.argv[1:])
