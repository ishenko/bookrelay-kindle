"""Exercise a relay-served JPEG through the native GTK card and capture it."""

import os
import subprocess
import sys
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
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            nonlocal active, peak
            stress_request = self.path != EXPECTED_PATH
            with lock:
                requests.append((self.path, self.headers.get("Authorization")))
                if stress_request:
                    active += 1
                    peak = max(peak, active)
            if self.path != EXPECTED_PATH and not any(
                self.path == f"/v1/books/{i}/cover?path=%2Fi%2F98%2F{i}%2Fcover.jpg"
                for i in range(1000, 1012)
            ):
                self.send_error(404)
            else:
                time.sleep(0.08)
                self.send_response(200)
                self.send_header("Content-Type", "image/jpeg")
                self.send_header("Content-Length", str(len(jpeg)))
                self.end_headers()
                self.wfile.write(jpeg)
            with lock:
                if stress_request:
                    active -= 1

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        env = dict(os.environ, BOOKRELAY_TEST_DPI="300",
                   BOOKRELAY_TEST_COVER_RELAY=f"http://127.0.0.1:{server.server_port}")
        subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1264x1680x24", binary, output],
                       check=True, env=env, timeout=40)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    assert len(requests) == 13, requests
    assert requests[0] == (EXPECTED_PATH, "Bearer smoke-test-token"), requests
    assert all(auth == "Bearer smoke-test-token" for _, auth in requests), requests
    assert peak <= 3, peak
    assert (Path(output) / "book-list-with-cover.png").is_file()


if __name__ == "__main__":
    main(*sys.argv[1:])
