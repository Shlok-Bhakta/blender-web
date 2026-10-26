# SPDX-License-Identifier: GPL-2.0-or-later
"""urllib3 transport for Blender's dedicated interpreter worker."""
import json
from email.parser import Parser
from _bpy_ghost_web_http import request as _request


class _RequestError(Exception):
    def __init__(self, message=None, *, request=None, response=None):
        super().__init__(message)
        self.message, self.request, self.response = message, request, response


class _TimeoutError(_RequestError):
    pass


def send_request(request):
    # Keep urllib3's original connection and response classes. Browser HTTP
    # handles TLS, redirects, decompression, CORS and forbidden wire headers.
    from urllib3.contrib.emscripten.response import EmscriptenResponse
    body = request.body
    if hasattr(body, 'read'):
        body = body.read()
    if isinstance(body, str):
        body = body.encode('utf-8')
    elif body is not None and not isinstance(body, (bytes, bytearray, memoryview)):
        body = b''.join(part.encode('utf-8') if isinstance(part, str) else part for part in body)
    metadata, data = _request(request.method, request.url, json.dumps(request.headers),
                              body, request.timeout or 0)
    metadata = json.loads(metadata)
    if 'error' in metadata:
        error = _TimeoutError if metadata.get('timeout') else _RequestError
        raise error(metadata['error'], request=request)
    request.url = metadata['url'] or request.url
    headers = {key.lower(): value for key, value in Parser().parsestr(metadata['headers']).items()}
    return EmscriptenResponse(status_code=metadata['status'],
        headers=headers, body=data, request=request)


def send_streaming_request(request):
    # Use urllib3's buffered fallback. This adapter does not claim network
    # streaming: its response reader can split the completed buffered body.
    return None


def prepare_downloader_response(response):
    """Give Blender's raw downloader the browser's decoded byte count."""
    if response.request.method.upper() == 'HEAD' or response.status_code in (204, 304):
        return
    body = response.raw._response.body
    if not isinstance(body, bytes):
        raise TypeError('Expected a buffered browser HTTP response')
    response.headers.pop('Content-Encoding', None)
    response.headers['Content-Length'] = str(len(body))
