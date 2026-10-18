#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Local static host. --pages-mode omits isolation headers to exercise the worker workaround."""
import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=Path("build/native/dist"),
                        help="Static output directory; defaults to the native Blender application")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--pages-mode", action="store_true")
    args = parser.parse_args()
    if not (args.directory / "manifest.json").is_file():
        parser.error(f"Missing manifest.json in {args.directory}. Build the native application with "
                     "python3 web/tools/native_build.py --app, or select another built site with --directory")

    class Handler(SimpleHTTPRequestHandler):
        extensions_map = {**SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm", ".js": "text/javascript"}

        def end_headers(self):
            if not args.pages_mode:
                self.send_header("Cross-Origin-Opener-Policy", "same-origin")
                self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
                self.send_header("Cross-Origin-Resource-Policy", "same-origin")
            # The service worker owns versioned caching. Do not stale-cache its script.
            self.send_header("Cache-Control", "no-cache")
            super().end_headers()

    server = ThreadingHTTPServer((args.bind, args.port), partial(Handler, directory=str(args.directory)))
    print(f"Serving {args.directory.resolve()} at http://{args.bind}:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
