# Copyright 2022 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""Console HTTP Log Server functions."""

import asyncio
import datetime
import email.utils
import logging
import mimetypes
import socket
import webbrowser
from pathlib import Path
from typing import Callable
from aiohttp import web, WSMsgType

from pw_console.web_kernel import WebKernel

_LOG = logging.getLogger(__package__)


def aiohttp_server(handler: Callable) -> web.AppRunner:
    app = web.Application()
    app.add_routes([web.get('/', handler), web.get('/{name}', handler)])
    runner = web.AppRunner(app)
    return runner


def find_available_port(start_port=8080, max_retries=100) -> int:
    """Finds the next available port starting from `start_port`."""
    for port in range(start_port, start_port + max_retries):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            try:
                s.bind(('localhost', port))
                return port
            except OSError:
                pass  # Port is already in use, try the next one

    raise RuntimeError(
        (
            'No available port found within the range '
            f'{start_port}-{start_port + max_retries}'
        )
    )


def pw_console_http_server(
    start_port: int, html_files: dict[str, str], kernel_params
) -> None:
    handler = WebHandlers(html_files=html_files, kernel_params=kernel_params)
    runner = aiohttp_server(handler.handle_request)
    port = find_available_port(start_port)
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(runner.setup())
    site = web.TCPSite(runner, 'localhost', port)
    loop.run_until_complete(site.start())
    url = f'http://localhost:{port}'
    print(url)
    webbrowser.open(url)
    loop.run_forever()


class WebHandlers:
    """Request handler that serves files from pw_console.html package data."""

    def __init__(self, html_files: dict[str, str], kernel_params):
        self.html_files = html_files
        self.date_modified = email.utils.formatdate(
            datetime.datetime.now().timestamp(), usegmt=True
        )
        self.kernel_params = kernel_params

    async def handle_request(
        self, request: web.Request
    ) -> web.Response | web.WebSocketResponse:
        _LOG.debug(
            '%s: %s',
            request.remote,
            request.raw_path,
        )

        path = request.path
        if path == '/':
            path = '/console.html'

        if path == '/ws':
            return await self.handle_websocket(request)

        if path not in self.html_files:
            return web.Response(status=404, text='File not found')

        content: bytes = self.html_files[path].encode('utf-8')
        content_type = 'application/octet-stream'
        mime_guess, _ = mimetypes.guess_type(Path(path).name)
        if mime_guess:
            content_type = mime_guess

        return web.Response(
            body=content,
            content_type=content_type,
            headers={
                'Content-Length': str(len(content)),
                'Last-Modified': self.date_modified,
            },
        )

    async def handle_websocket(self, request) -> web.WebSocketResponse:
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        kernel = WebKernel(ws, self.kernel_params)
        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                if msg.data == 'close':
                    kernel.handle_disconnect()
                    await ws.close()
                else:
                    response = await kernel.handle_request(msg.data)
                    await ws.send_str(response)
            elif msg.type == WSMsgType.ERROR:
                _LOG.warning(
                    'ws connection closed with exception: %s', ws.exception()
                )

        return ws
