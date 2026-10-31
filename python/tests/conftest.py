import re
import threading
from collections.abc import Iterator
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest


@dataclass
class RangeServer:
    """A local HTTP server for one directory that honours single Range requests."""

    url: str
    requests: list[tuple[str, str | None]] = field(default_factory=list)
    served_bytes: int = 0


def _handler(root: Path, server: RangeServer) -> type[BaseHTTPRequestHandler]:
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args) -> None:
            pass

        def _target(self) -> Path | None:
            path = (root / self.path.split("?", 1)[0].lstrip("/")).resolve()
            return path if path.is_file() and root in path.parents else None

        def _respond(self, body: bool) -> None:
            header = self.headers.get("Range")
            with lock:
                server.requests.append((self.command, header))
            path = self._target()
            if path is None:
                self.send_error(404)
                return
            data = path.read_bytes()
            etag = f'"{len(data)}-{path.stat().st_mtime_ns}"'
            if_match = self.headers.get("If-Match")
            if if_match and if_match != etag:
                self.send_error(412)
                return
            start, stop, status = 0, len(data), 200
            if header:
                match = re.fullmatch(r"bytes=(\d*)-(\d*)", header)
                if not match or match.groups() == ("", ""):
                    self.send_error(416)
                    return
                first, last = match.groups()
                if first:
                    start = int(first)
                    stop = min(int(last) + 1, len(data)) if last else len(data)
                else:
                    start = max(len(data) - int(last), 0)
                if start >= len(data):
                    self.send_error(416)
                    return
                status = 206
            self.send_response(status)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("ETag", etag)
            self.send_header("Content-Length", str(stop - start))
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{stop - 1}/{len(data)}")
            self.end_headers()
            if body:
                self.wfile.write(data[start:stop])
                with lock:
                    server.served_bytes += stop - start

        def do_GET(self) -> None:
            self._respond(body=True)

        def do_HEAD(self) -> None:
            self._respond(body=False)

    return Handler


@pytest.fixture
def range_server(tmp_path: Path) -> Iterator[RangeServer]:
    """Serves tmp_path over http://127.0.0.1 with Range support."""
    root = tmp_path.resolve()
    state = RangeServer(url="")
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), _handler(root, state))
    state.url = f"http://127.0.0.1:{httpd.server_address[1]}"
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    try:
        yield state
    finally:
        httpd.shutdown()
        httpd.server_close()
        thread.join()
