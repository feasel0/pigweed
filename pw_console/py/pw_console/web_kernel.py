# Copyright 2024 The Pigweed Authors
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
"""REPL Kernel for web console."""

import asyncio
import io
import json
import logging
import operator
import sys
import types
from typing import Callable, TYPE_CHECKING, List, Dict, Any

from prompt_toolkit.completion import CompleteEvent
from prompt_toolkit.document import Document
from ptpython.completer import PythonCompleter
from ptpython.repl import _has_coroutine_flag

from pw_console.python_logging import log_record_to_json

if TYPE_CHECKING:
    from pw_console.log_line import LogLine

_LOG = logging.getLogger(__package__)


class UnknownRequestType(Exception):
    """Exception for request with unknown or missing types."""


class UnknownRequestData(Exception):
    """Exception for request with missing data attributes."""


class MissingCallId(Exception):
    """Exception for request with missing call id."""


def format_completions(all_completions) -> List[Dict[str, str]]:
    # Hide private suggestions
    all_completions = [
        completion
        for completion in all_completions
        if not completion.text.startswith('_')
    ]

    return list(
        map(
            lambda x: {
                'text': x.text,
                'type': (
                    'keyword' if x.display[0][1].endswith('()') else 'variable'
                ),
            },
            all_completions,
        )
    )


def _format_result_output(result) -> str:
    """Return a plaintext repr of any object."""
    try:
        formatted_result = repr(result)
    except BaseException:  # pylint: disable=broad-exception-caught
        formatted_result = ''
        # Exception is handled below instead of here.
    return formatted_result


def compile_code(code: str, mode: str) -> types.CodeType:
    return compile(
        code,
        '<stdin>',
        mode,
        dont_inherit=True,
    )


class WebSocketStreamingResponder(logging.Handler):
    def __init__(self, connection):
        logging.Handler.__init__(self=self)
        self.connection = connection
        self.request_ids = []
        self.formatter: Callable[[LogLine], str] = operator.attrgetter(
            'ansi_stripped_log'
        )
        self.formatter = lambda record: json.loads(log_record_to_json(record))

    def emit(self, record):
        log_string = self.formatter(record)
        self._process_log_string(log_string)

    def _process_log_string(self, log_string):
        loop = asyncio.get_event_loop()
        asyncio.set_event_loop(loop)
        for req_id in self.request_ids:
            loop.create_task(
                self.connection.send_str(
                    json.dumps(
                        {
                            'id': req_id,
                            'streaming': True,
                            'data': {'log_line': log_string},
                        }
                    )
                )
            )


class WebKernel:
    """Web Kernel implementation."""

    def __init__(self, connection, kernel_params):
        self.connection = connection
        self.kernel_params = kernel_params
        self.logger_handlers = {}
        self.connected = False
        self.python_completer = PythonCompleter(
            self.get_globals,
            self.get_locals,
            lambda: True,
        )

    async def handle_request(self, request) -> str:
        """Handle the request from web browser."""
        try:
            parsed_request = json.loads(request)
            request_type = parsed_request.get('type', None)
            request_data = parsed_request.get('data', None)
            call_id = parsed_request.get('id', None)
            if request_type is None:
                raise UnknownRequestType(
                    'Unknown request type: {}'.format(parsed_request)
                )
            if request_data is None:
                raise UnknownRequestData(
                    'Unknown request data: {}'.format(parsed_request)
                )

            if call_id is None:
                raise MissingCallId(
                    'Missing call id: {}'.format(parsed_request)
                )

            if request_type == 'autocomplete':
                try:
                    completions = self.handle_autocompletion(
                        request_data['code'],
                        request_data['cursor_pos'],
                    )
                    return json.dumps({'id': call_id, 'data': completions})
                except KeyError as error:
                    raise KeyError(
                        (
                            'Missing data.code or data.cursor_pos attributes:'
                            '{}'.format(request_data)
                        )
                    ) from error

            if request_type == 'eval':
                try:
                    result = await self.handle_eval(request_data['code'])
                    return json.dumps({'id': call_id, 'data': result})
                except KeyError as error:
                    raise KeyError(
                        'Missing data.code attributes: {}'.format(request_data)
                    ) from error

            if request_type == 'log_source_list':
                sources = self.handle_log_source_list()
                return json.dumps({'id': call_id, 'data': sources})
            if request_type == 'log_source_subscribe':
                try:
                    # Close requests have same call id with just .close = true
                    if 'close' in parsed_request:
                        has_unsubbed = self.handle_log_source_unsubscribe(
                            request_data['name'], call_id
                        )
                        return json.dumps(
                            {
                                'id': call_id,
                                'streaming': True,
                                'data': has_unsubbed,
                            }
                        )
                    has_subscribed = self.handle_log_source_subscribe(
                        request_data['name'], call_id
                    )
                    return json.dumps(
                        {
                            'id': call_id,
                            'streaming': True,
                            'data': has_subscribed,
                        }
                    )
                except KeyError as error:
                    raise KeyError(
                        'Missing data.name attributes: {}'.format(request_data)
                    ) from error

            return 'unknown'
        except ValueError as error:
            _LOG.error('Failed to parse request: %s', request)
            return ''

    async def handle_eval(self, code: str) -> Dict[str, str] | None:
        """Evaluate user code and return output."""
        # Patch stdout and stderr to capture repl print() statements.
        temp_stdout = io.StringIO()
        temp_stderr = io.StringIO()
        original_stdout = sys.stdout
        original_stderr = sys.stderr

        sys.stdout = temp_stdout
        sys.stderr = temp_stderr

        def return_result_with_stdout_stderr(result) -> Dict[str, str]:
            # Always restore original stdout and stderr
            sys.stdout = original_stdout
            sys.stderr = original_stderr

            return {
                'result': (
                    _format_result_output(result) if not result is None else ''
                ),
                'stdout': temp_stdout.getvalue(),
                'stderr': temp_stderr.getvalue(),
            }

        try:
            result = await self._eval_async(code)
        except KeyboardInterrupt:
            return_result_with_stdout_stderr(None)
            raise
        except SystemExit:
            return None
        except BaseException as e:  # pylint: disable=broad-exception-caught
            return return_result_with_stdout_stderr(_format_result_output(e))
        else:
            # Print.
            return return_result_with_stdout_stderr(result)

    async def _eval_async(self, code: str) -> Any:
        """
        Evaluate the code and return result
        """

        # WORKAROUND: Due to a bug in Jedi, the current directory is removed
        # from sys.path. See: https://github.com/davidhalter/jedi/issues/1148
        if '' not in sys.path:
            sys.path.insert(0, '')

        # Try eval first
        try:
            compiled_code = compile_code(code, 'eval')
        except SyntaxError:
            pass
        else:
            # No syntax errors for eval. Do eval.
            result = eval(  # pylint: disable=eval-used
                code, self.get_globals(), self.get_locals()
            )

            if _has_coroutine_flag(compiled_code):
                result = await result

            return result

        # If not a valid `eval` expression, compile as `exec` expression
        compiled_code = compile_code(code, 'exec')
        result = eval(  # pylint: disable=eval-used
            compiled_code, self.get_globals(), self.get_locals()
        )

        if _has_coroutine_flag(compiled_code):
            result = await result
            return result

        return

    def handle_autocompletion(
        self, code: str, cursor_pos: int
    ) -> List[Dict[str, str]]:
        doc = Document(code, cursor_pos)
        all_completions = list(
            self.python_completer.get_completions(
                doc,
                CompleteEvent(completion_requested=False, text_inserted=True),
            )
        )
        return format_completions(all_completions)

    def handle_disconnect(self) -> None:
        _LOG.info('pw_console.web_kernel disconnecting.')
        self.connected = False

    def get_globals(self) -> dict[str, Any] | None:
        if self.kernel_params['global_vars']:
            return self.kernel_params['global_vars']
        return None

    def get_locals(self) -> dict[str, Any] | None:
        if self.kernel_params['local_vars']:
            return self.kernel_params['local_vars']

        return self.get_globals()

    def handle_log_source_list(self) -> List[str]:
        if self.kernel_params['loggers']:
            return list(self.kernel_params['loggers'].keys())
        return []

    def handle_log_source_subscribe(self, logger_name, request_id) -> bool:
        if self.kernel_params['loggers'][logger_name]:
            if not logger_name in self.logger_handlers:
                self.logger_handlers[logger_name] = WebSocketStreamingResponder(
                    self.connection
                )
                for logger in self.kernel_params['loggers'][logger_name]:
                    logger.addHandler(self.logger_handlers[logger_name])
            self.logger_handlers[logger_name].request_ids.append(request_id)
            return True
        return False

    def handle_log_source_unsubscribe(self, logger_name, request_id) -> bool:
        if (
            self.kernel_params['loggers'][logger_name]
            and self.logger_handlers[logger_name]
        ):
            self.logger_handlers[logger_name].request_ids.remove(request_id)
            # Remove handler if all requests have unsubscribed
            if len(self.logger_handlers[logger_name].request_ids) == 0:
                for logger in self.kernel_params['loggers'][logger_name]:
                    logger.removeHandler(self.logger_handlers[logger_name])
                del self.logger_handlers[logger_name]
            return True
        return False
