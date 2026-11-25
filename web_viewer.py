"""Serve the Three.js viewer and explicitly opened GLBs on this computer."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import secrets
import shutil
from threading import Thread
from urllib.parse import quote, urlsplit

from glb_info import triangle_count


ASSETS = Path(__file__).resolve().with_name("viewer")
TYPES = {".js": "text/javascript", ".css": "text/css", ".html": "text/html; charset=utf-8"}


class ViewerHandler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.serve(head=False)

    def do_HEAD(self):
        self.serve(head=True)

    def serve(self, head):
        if self.headers.get("Host") != f"127.0.0.1:{self.server.server_port}":
            self.send_error(403)
            return
        route = urlsplit(self.path).path
        model = self.server.models.get(route)
        path = model or self.server.assets.get(route)
        if path is None:
            self.send_error(404)
            return
        try:
            stream = path.open("rb")
        except OSError:
            self.send_error(404)
            return
        with stream:
            self.send_response(200)
            self.send_header("Content-Type", "model/gltf-binary" if model else TYPES[path.suffix])
            self.send_header("Content-Length", str(os.fstat(stream.fileno()).st_size))
            self.send_header("Cache-Control", "no-store" if model else "no-cache")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Referrer-Policy", "no-referrer")
            if model:
                self.send_header("X-Model-Name", quote(path.name, safe=""))
            elif path.suffix == ".html":
                self.send_header("Content-Security-Policy", "default-src 'none'; "
                                 "script-src 'self' 'unsafe-inline'; style-src 'self'; "
                                 "connect-src 'self' blob: data:; img-src 'self' blob: data:; "
                                 "frame-ancestors 'none'; base-uri 'none'")
            try:
                self.end_headers()
                if not head:
                    shutil.copyfileobj(stream, self.wfile, length=256 * 1024)
            except (ConnectionError, TimeoutError):
                pass


class LocalViewer:
    def __init__(self):
        self.server = None
        self.thread = None
        self.tokens = {}

    def url_for(self, path):
        path = Path(path).resolve()
        triangle_count(path)
        if self.server is None:
            assets = {"/" + file.relative_to(ASSETS).as_posix(): file for file in ASSETS.rglob("*")
                      if file.is_file() and file.suffix in TYPES}
            assets["/"] = ASSETS / "index.html"
            self.server = ThreadingHTTPServer(("127.0.0.1", 0), ViewerHandler)
            self.server.assets = assets
            self.server.models = {}
            self.thread = Thread(target=self.server.serve_forever, args=(0.1,), daemon=True)
            self.thread.start()
        if path not in self.tokens:
            token = secrets.token_urlsafe(24)
            self.server.models["/models/" + token] = path
            self.tokens[path] = token
        return f"http://127.0.0.1:{self.server.server_port}/?model={self.tokens[path]}"

    def close(self):
        if self.server is not None:
            self.server.shutdown()
            self.server.server_close()
            self.thread.join()
            self.server = self.thread = None
            self.tokens.clear()
