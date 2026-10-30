#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
import serial
import threading
import time
import os
import re
import sys
import zlib
import socket
import argparse
import asyncio
import base64
import queue
from collections import deque
from loguru import logger
from rttt.console import Console
from rttt.connectors import FileLogConnector
from rttt.connectors.base import Connector
from rttt.connectors.middleware import AsyncMiddleware, Middleware
from rttt.event import Event, EventType
from rttt.utils import parse_listen

try:
    import fcntl
    import termios
except ImportError:  # not on Windows
    fcntl = termios = None

try:
    from west.commands import WestCommand
except ImportError:
    if __name__ == '__main__':
        class WestCommand:
            def __init__(self, name, help, description):
                self.name = name
                self.help = help
                self.description = description
    else:
        raise

DEFAULT_HISTORY_FILE = os.path.expanduser(f"~/.serial_console_history")
DEFAULT_CONSOLE_FILE = os.path.expanduser(f"~/.serial_console_console")
DEFAULT_MODEM_TRACE_FILE = os.path.expanduser(f"~/.serial_console.mtrace")
DEFAULT_MCP_LISTEN = '127.0.0.1:8095'
PASSPHRASE_ENV = 'SERIAL_CONSOLE_PASSPHRASE'
_HEX8 = re.compile(r'[0-9A-Fa-f]{8}')


def split_crc(line: str):
    """Split the "\\t<CRC32 hex>" the ATCI appends in CRC mode off a line.

    Returns (text, crc), crc None when the line carries none. A line that
    is only the tab and the CRC (what follows an error the command printed
    itself) gives an empty text.
    """
    if len(line) >= 9 and line[-9] == '\t' and _HEX8.fullmatch(line[-8:]):
        return line[:-9], int(line[-8:], 16)
    return line, None


class Line(str):
    """A device output line as shown, plus the text it had on the wire (CRC
    stripped, before unescaping) and the CRC the device appended to it."""

    wire: str
    crc: 'int | None'

    def __new__(cls, text, wire=None, crc=None):
        line = super().__new__(cls, text)
        line.wire = text if wire is None else wire
        line.crc = crc
        return line

    @property
    def raw(self):
        """The line exactly as received, CRC included."""
        return self.wire if self.crc is None else f'{self.wire}\t{self.crc:08X}'

    def crc_ok(self):
        """Whether the line's own CRC matches, None when it has none."""
        if self.crc is None:
            return None
        return zlib.crc32(self.wire.encode('utf-8')) == self.crc


class BaseConsoleConnector(Connector):
    """Base class for console connectors with shared logic."""

    def __init__(self) -> None:
        super().__init__()
        self._thread_read = None
        self._thread_line = None
        self.is_running = False
        self.lines = queue.Queue()
        self.modem_trace_fd = None

    def set_modem_trace_file(self, modem_trace_file: str):
        """
        Set the file where modem trace data will be written.
        This method is called by the Console to set the modem trace file.
        """
        logger.info(f"Modem trace file set to: {modem_trace_file}")
        self.modem_trace_fd = open(modem_trace_file, 'wb')

    def _start_threads(self):
        """Start the read and line processing threads."""
        self.is_running = True
        self._thread_read = threading.Thread(target=self._read_task, daemon=True)
        self._thread_read.start()
        self._thread_line = threading.Thread(target=self._line_task, daemon=True)
        self._thread_line.start()

    def close(self):
        logger.info("Closing connection")
        if not self.is_running:
            return
        self.is_running = False
        if self._thread_read:
            self._thread_read.join()
        if self._thread_line:
            self._thread_line.join()
        self._close_resources()
        if self.modem_trace_fd:
            self.modem_trace_fd.close()
        self._emit(Event(EventType.CLOSE, ''))
        logger.info("Connection closed")

    def _close_resources(self):
        """Override to close specific resources (serial port, file, etc.)."""
        pass

    def _read_task(self):
        """Override to implement specific read logic."""
        raise NotImplementedError

    def _line_task(self):
        while self.is_running:
            try:
                line, crc = split_crc(self.lines.get(timeout=1))

                if line.startswith("@MT: "):
                    # Format: @MT: <remaining_bytes>,"<base64_data>"
                    if self.modem_trace_fd:
                        index = line.find(',')
                        b64text = line[index + 2:-1]  # skip ',"' prefix and '"' suffix
                        if b64text:
                            data = base64.b64decode(b64text)
                            self.modem_trace_fd.write(data)
                elif line.startswith("@LOG: "):
                    # Format: @LOG: "<message>"
                    self._emit(Event(EventType.LOG, line[7:-1].encode('utf-8').decode('unicode_escape')))
                else:
                    text = line.encode('utf-8').decode('unicode_escape')
                    self._emit(Event(EventType.OUT, Line(text, wire=line, crc=crc)))
            except queue.Empty:
                continue
            except Exception as e:
                logger.error(f"Error processing line: {e}")


class SerialConnector(BaseConsoleConnector):
    """Serial port connector. The port is opened exclusively, reopened when
    the USB serial device comes back after a disconnect, and can be released
    for another tool and taken back (`release` / `acquire`)."""

    RECONNECT_INTERVAL = 1.0
    # The ATCI UART sleeps when idle and the first bytes only wake it up,
    # so after `wake_idle` seconds without sending, an ESC (which just
    # clears the device's command buffer) goes first, `WAKE_DELAY` ahead.
    WAKE_DELAY = 0.1

    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 0.2, wake_idle: float = 2.0) -> None:
        super().__init__()
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.wake_idle = wake_idle
        self.ser = None
        self.state = 'closed'
        self.error = ''
        self._cache = ''
        self._port_lock = threading.Lock()
        self._released = False
        self._last_tx = 0.0
        self._next_reopen = 0.0

    def _open_port(self):
        ser = serial.Serial(self.port, self.baudrate, timeout=self.timeout, exclusive=True)
        if fcntl is not None:
            # exclusive=True is only an advisory flock, which screen, minicom
            # or a plain pyserial open ignore. TIOCEXCL makes their open fail
            # with "busy" instead of two readers splitting the stream.
            try:
                fcntl.ioctl(ser.fileno(), termios.TIOCEXCL)
            except OSError as e:
                logger.warning(f"TIOCEXCL failed on {self.port}: {e}")
        return ser

    @staticmethod
    def _close_port(ser):
        """Close, dropping TIOCEXCL first: it belongs to the tty, not to this
        open file, and would otherwise outlive the close while anything else
        still has the device open."""
        if fcntl is not None:
            try:
                fcntl.ioctl(ser.fileno(), termios.TIOCNXCL)
            except Exception:
                pass
        try:
            ser.close()
        except Exception:
            pass

    def _set_state(self, state: str, error: str = ''):
        self.state = state
        self.error = error
        # The console shows any status but "connected" as a fault overlay;
        # a release is explained through the error text.
        status = 'connected' if state == 'connected' else 'disconnected'
        self._emit(Event(EventType.CONN, {'source': 'serial', 'status': status, 'error': error}))

    def open(self):
        logger.info(f"Opening serial port {self.port} at {self.baudrate} baud")
        self.ser = self._open_port()
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
        self._start_threads()
        self._emit(Event(EventType.OPEN, ''))
        self._set_state('connected')
        logger.info("Serial connection opened")

    def _close_resources(self):
        with self._port_lock:
            ser, self.ser = self.ser, None
        if ser:
            self._close_port(ser)

    def release(self):
        """Close the port so another tool can open it; no automatic reopen."""
        with self._port_lock:
            self._released = True
            ser, self.ser = self.ser, None
        if ser:
            self._close_port(ser)
        logger.info(f"Serial port {self.port} released")
        self._set_state('released', f'{self.port} is released for another tool; '
                                    'the MCP port_acquire tool takes it back')

    def acquire(self):
        """Open the port again after `release` (or a disconnect). Raises when
        it cannot be opened, e.g. another tool still holds it."""
        with self._port_lock:
            if self.ser is None:
                self.ser = self._open_port()
            self._released = False
        logger.info(f"Serial port {self.port} acquired")
        self._set_state('connected')

    def send(self, text: str) -> bool:
        """Write one command line; False when the port is not open."""
        with self._port_lock:
            if self.ser is None:
                return False
            try:
                if self.wake_idle > 0 and time.monotonic() - self._last_tx > self.wake_idle:
                    self.ser.write(b'\x1b')
                    time.sleep(self.WAKE_DELAY)
                self.ser.write(f'\x1b{text}\n'.encode('utf-8'))
            except (serial.SerialException, OSError) as e:
                logger.warning(f"Serial write error: {e}")
                return False
            self._last_tx = time.monotonic()
            return True

    def handle(self, event: Event):
        logger.info(f'handle: {event.type} {event.data}')
        if event.type == EventType.IN:
            if not self.send(event.data):
                logger.warning(f"Not sent, serial port is {self.state}: {event.data}")
        self._emit(event)

    def _reopen(self):
        """Reopen the port after it went away, at most every RECONNECT_INTERVAL."""
        now = time.monotonic()
        if now < self._next_reopen:
            return
        self._next_reopen = now + self.RECONNECT_INTERVAL
        # COM ports are not files; there the open attempt is the only check.
        if os.name != 'nt' and not os.path.exists(self.port):
            return
        try:
            ser = self._open_port()
        except (serial.SerialException, OSError) as e:
            logger.debug(f"Reopen of {self.port} failed: {e}")
            return
        with self._port_lock:
            if self._released or self.ser is not None:
                self._close_port(ser)
                return
            self.ser = ser
        logger.info(f"Serial port {self.port} reopened")
        self._set_state('connected')

    def _port_lost(self, ser, error):
        with self._port_lock:
            if self.ser is ser:
                self.ser = None
            released = self._released
        self._close_port(ser)
        if not released and self.is_running:
            logger.warning(f"Serial port lost: {error}")
            self._set_state('disconnected', f'{self.port}: {error}')
            self._next_reopen = time.monotonic() + self.RECONNECT_INTERVAL

    def _read_task(self):
        current = None
        while self.is_running:
            ser = self.ser
            if ser is None:
                if not self._released:
                    self._reopen()
                time.sleep(0.1)
                continue
            if ser is not current:
                # A half line from before a reconnect never gets finished.
                current = ser
                self._cache = ''
            try:
                data = ser.read(ser.in_waiting or 1)
            except Exception as e:
                self._port_lost(ser, e)
                continue
            if not data:
                continue
            text = self._cache + data.decode('utf-8', errors='backslashreplace')
            while True:
                newline_index = text.find('\n')
                if newline_index == -1:
                    self._cache = text
                    break
                # CRC stays on; the line task splits it off.
                self.lines.put(text[:newline_index].rstrip('\r'))
                text = text[newline_index + 1:]


class FileConnector(BaseConsoleConnector):

    def __init__(self, filename: str) -> None:
        super().__init__()
        self.filename = filename

    def open(self):
        logger.info(f"Opening file {self.filename}")
        if not os.path.exists(self.filename):
            logger.error(f"File {self.filename} does not exist")
            return
        self._start_threads()
        self._emit(Event(EventType.OPEN, ''))

    def handle(self, event: Event):
        logger.info(f'handle: {event.type} {event.data}')
        self._emit(event)

    def _read_task(self):
        linetmp = ''
        with open(self.filename, 'r', encoding='utf-8') as file:
            while self.is_running:
                try:
                    line = file.readline()
                    if not line:
                        time.sleep(0.1)
                        continue
                    line = line.rstrip('\r\n')
                    if linetmp:
                        line = linetmp + line
                        linetmp = ''

                    if len(line) > 10 and line[-9] == '\t':
                        line = line[:-9]  # remove CRC

                    if line.startswith("@MT: ") and not line.endswith('"'):
                        linetmp = line
                        continue

                    self.lines.put(line)

                except Exception as e:
                    logger.warning(f"File read error: {e}")
                    break


class InputMiddleware(Middleware):
    """Sends the initial input once the connection is open: a string, or
    each line of a file, waiting for OK after each.

    A middleware rather than a handler on the connector: a connector takes
    one handler, and the next layer up needs it.
    """

    def __init__(self, connector: Connector, arg: str):
        super().__init__(connector)
        logger.info(f"Initial input: {arg}")
        self.arg = arg
        self.signal = threading.Event()

    def _task(self):
        arg = self.arg
        time.sleep(0.5)
        if os.path.exists(arg):
            logger.info(f'Starting input task for file: {arg}')
            try:
                with open(arg, 'r', encoding='utf-8') as f:
                    for line in f:
                        line = line.strip()
                        if line:
                            logger.info(f'Sending line: {line}')
                            self.signal.clear()
                            self.handle(Event(EventType.IN, line))
                            if not self.signal.wait(timeout=1):
                                logger.warning("Timeout waiting for OK")
                            # time.sleep(0.01)  # optional
                logger.info('Done input task from file')
            except Exception as e:
                logger.error(f"Failed to read file {arg}: {e}")
        else:
            logger.info('Starting input task')
            time.sleep(0.5)
            self.handle(Event(EventType.IN, arg))
            logger.info('Done input task')

    def _on(self, event: Event):
        self._emit(event)
        if event.type == EventType.OPEN:
            threading.Thread(target=self._task, daemon=True).start()
        elif event.type == EventType.OUT and event.data == 'OK':
            self.signal.set()


class _Buffer:
    """Ring buffer of lines with a running cursor (total lines ever added)."""

    def __init__(self, size: int):
        self.lines = deque(maxlen=size)
        self.total = 0

    def add(self, line):
        self.lines.append(line)
        self.total += 1

    def since(self, cursor: int) -> list:
        lines = list(self.lines)
        return lines[max(0, cursor - (self.total - len(lines))):]

    def read(self, after_cursor: int = -1, lines: int = -1, pattern: str = ''):
        """Regex-filtered lines and the cursor to continue from.

        With `after_cursor` the lines after it, oldest first, at most `lines`
        of them (-1 = no limit); the cursor then points right after the last
        line returned, so reading on from it misses nothing. Without it the
        last `lines` lines (-1 = all) and the cursor at the end.
        Raises re.error on a bad pattern.
        """
        buffered = list(self.lines)
        base = self.total - len(buffered)
        items = list(enumerate(buffered, base))
        if after_cursor >= 0:
            items = items[max(0, after_cursor - base):]
        if pattern:
            regex = re.compile(pattern, re.IGNORECASE)
            items = [(pos, line) for pos, line in items if regex.search(line)]
        cursor = self.total
        if after_cursor >= 0:
            if 0 <= lines < len(items):
                items = items[:lines]
                cursor = items[-1][0] + 1 if items else max(after_cursor, base)
        elif lines >= 0:
            items = items[-lines:] if lines > 0 else []
        return [line for _, line in items], cursor


class _BearerAuth:
    """ASGI wrapper requiring `Authorization: Bearer <token>` on every request."""

    def __init__(self, app, token: str):
        self.app = app
        self.token = token

    async def __call__(self, scope, receive, send):
        if scope['type'] == 'http':
            headers = dict(scope.get('headers') or [])
            if headers.get(b'authorization', b'').decode('latin-1') != f'Bearer {self.token}':
                from starlette.responses import JSONResponse
                await JSONResponse({'status': 'error', 'error': 'unauthorized'}, status_code=401)(scope, receive, send)
                return
        await self.app(scope, receive, send)


class MCPPortInUseError(Exception):
    pass


_RESULT_OK = 'OK'
_PERMISSION_DENIED = 'ERROR: "Permission denied"'
# Login is refused this long after boot (CONFIG_HIO_ATCI_LOGIN_BOOT_DELAY).
_LOGIN_BOOT_DELAY = 6.0

_INSTRUCTIONS = """\
Serial console of a HARDWARIO device running the ATCI (AT command interface).
- `at_command` sends one AT command and returns its response and final result;
  `at_batch` runs a list of them.
- Unsolicited lines starting with "@" (e.g. @BOOT) are URCs: `read_urc`, or
  `wait_for` to block until one arrives. Device logs (@LOG) are in `read_log`.
- Pass the cursors an AT result returns to `read_urc` / `read_log` / `wait_for`
  to see only what came after the command.
- Commands that reboot the device (AT$REBOOT) may return no result; wait for
  @BOOT with `wait_for` and `after_cursor`.
- `port_release` hands the serial port to another tool, `port_acquire` takes it back.
"""


def _make_mcp_server(name: str, host: str, port: int):
    """An MCP server from whichever SDK rttt brought in, with that SDK's
    ToolAnnotations and a factory of its ASGI app: mcp v2 (MCPServer) or v1
    (FastMCP, which rttt up to 1.11 ships). Both guard /mcp against DNS
    rebinding on a loopback host."""
    try:
        from mcp.server.mcpserver import MCPServer
        from mcp_types import ToolAnnotations
    except ImportError:
        from mcp.server.fastmcp import FastMCP
        from mcp.types import ToolAnnotations as V1Annotations
        server = FastMCP(name, instructions=_INSTRUCTIONS, host=host, port=port,
                         log_level='CRITICAL', stateless_http=True)

        def annotations(**hints):
            # v1 names the fields in camelCase and silently keeps unknown
            # snake_case ones as extras, leaving the real hints unset.
            return V1Annotations(**{re.sub(r'_(\w)', lambda m: m[1].upper(), k): v for k, v in hints.items()})

        return server, annotations, server.streamable_http_app
    server = MCPServer(name, instructions=_INSTRUCTIONS, log_level='CRITICAL')
    return server, ToolAnnotations, lambda: server.streamable_http_app(stateless_http=True, host=host)


class SerialMCPServer(AsyncMiddleware):
    """MCP server (Streamable HTTP) for AI agents on top of the serial console:
    AT commands with their responses, URCs and device logs."""

    def __init__(self, connector: SerialConnector, listen: str = DEFAULT_MCP_LISTEN, token: str = None,
                 passphrase: str = None, max_lines: int = 5000):
        super().__init__(connector)
        self.host, self.port = parse_listen(listen, default_port=8095)
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind((self.host, self.port))
        except OSError as e:
            raise MCPPortInUseError(f'{self.host}:{self.port}') from e
        finally:
            sock.close()
        self.token = token
        self.passphrase = passphrase
        self._terminal = _Buffer(max_lines)
        self._output = _Buffer(max_lines)
        self._urc = _Buffer(max_lines)
        self._log = _Buffer(max_lines)
        self._new_line = asyncio.Event()
        self._at_lock = asyncio.Lock()
        self._auth = 'unknown'
        self._login_failed = False
        self._boot_time = None
        self._crc_seen = False
        # A command that timed out: the device still answers it before the
        # next one, the ATCI runs commands one at a time.
        self._pending = None
        self._login_task = None
        self._mcp, self._annotations, self._make_app = _make_mcp_server('Serial Console', self.host, self.port)
        self._register_tools()
        self._server = None
        self._server_task = None

    async def _serve(self):
        import uvicorn

        app = self._make_app()
        if self.token:
            app = _BearerAuth(app, self.token)
        self._server = uvicorn.Server(uvicorn.Config(app, host=self.host, port=self.port, log_level='critical'))
        await self._server.serve()

    def open(self):
        super().open()
        self._server_task = asyncio.run_coroutine_threadsafe(self._serve(), self._loop)
        logger.info(f'MCP server starting on http://{self.host}:{self.port}/mcp')

    def close(self):
        if self._server_task:
            if self._server is not None:
                self._server.should_exit = True
            try:
                self._server_task.result(timeout=5)
            except Exception:
                self._server_task.cancel()
        super().close()

    @staticmethod
    def _is_final(text: str) -> bool:
        return text == _RESULT_OK or text.startswith('ERROR')

    def _is_urc(self, line) -> bool:
        # In CRC mode a URC carries its own CRC, response lines carry none.
        text = line.strip()
        return text.startswith('@') or (getattr(line, 'crc', None) is not None and text != '' and not self._is_final(text))

    async def _process(self, event: Event):
        if event.type == EventType.OUT:
            line = event.data if isinstance(event.data, Line) else Line(str(event.data))
            self._terminal.add({'direction': 'out', 'text': str(line)})
            self._output.add(line)
            if line.crc is not None:
                self._crc_seen = True
            if self._is_urc(line):
                self._urc.add(line)
                if line.startswith('@BOOT'):
                    self._auth = 'unknown'
                    self._login_failed = False
                    self._boot_time = asyncio.get_running_loop().time()
                    # A command cut off by the reboot never gets its result.
                    self._pending = None
                    self._schedule_login()
            self._new_line.set()
        elif event.type == EventType.CONN and event.data.get('status') == 'connected':
            # Opened, reopened after an unplug, or taken back: whatever the
            # device was answering is lost with the link, and so may the login.
            self._pending = None
            self._auth = 'unknown'
            self._schedule_login()
        elif event.type == EventType.IN:
            self._terminal.add({'direction': 'in', 'text': str(event.data)})
        elif event.type == EventType.LOG:
            self._log.add(event.data)
            self._new_line.set()

    async def _wait_new_line(self, deadline: float) -> bool:
        """Wait for any new device line until `deadline` (loop time); False on timeout.

        Everything runs on this loop, so nothing can arrive between a scan
        of the buffers and this clear().
        """
        remaining = deadline - asyncio.get_running_loop().time()
        if remaining <= 0:
            return False
        self._new_line.clear()
        try:
            await asyncio.wait_for(self._new_line.wait(), timeout=remaining)
            return True
        except asyncio.TimeoutError:
            return False

    async def _transact(self, command: str, timeout: float, echo: str = None) -> dict:
        """Send `command` and collect the lines up to its final result.

        URCs and logs that arrive meanwhile stay out of the response; they
        are in their own buffers.
        """
        loop = asyncio.get_running_loop()
        # Taken before sending: an answer can be on the loop before the
        # executor returns. Nothing else is due, a pending command is
        # finished first (see _at).
        cursor = self._output.total
        if not await loop.run_in_executor(None, self.connector.send, command):
            return {'status': 'error', 'error': f'serial port is {self.connector.state}: {self.connector.error}'}
        self.connector._emit(Event(EventType.IN, echo or command))
        body, final, cursor = await self._collect(cursor, loop.time() + timeout)
        crc_line = None
        if final is None:
            self._pending = {'command': command, 'cursor': cursor, 'body': body}
        elif final.crc is None and final.startswith('ERROR') and self._crc_seen:
            # An error the command printed itself ends with "\r\n"; in CRC
            # mode the CRC then follows on a line of its own.
            settle = loop.time() + 0.2
            while crc_line is None:
                later = [line for line in self._output.since(cursor) if not self._is_urc(line)]
                if later:
                    if later[0].strip() == '' and later[0].crc is not None:
                        crc_line = later[0]
                    break
                if not await self._wait_new_line(settle):
                    break
        return {'status': 'done', 'body': body, 'final': final, 'crc_line': crc_line}

    async def _collect(self, cursor: int, deadline: float):
        """Response lines from `cursor` up to the first final result, or
        until `deadline`. Returns (body, final or None, cursor after them)."""
        body = []
        while True:
            for line in self._output.since(cursor):
                cursor += 1
                if self._is_urc(line):
                    continue
                if self._is_final(line.strip()):
                    return body, line, cursor
                body.append(line)
            if not await self._wait_new_line(deadline):
                return body, None, cursor

    async def _finish_pending(self, timeout: float):
        """Wait up to `timeout` for the result of the command that timed out.
        Returns what came of it late, or None while it is still running."""
        pending = self._pending
        loop = asyncio.get_running_loop()
        body, final, pending['cursor'] = await self._collect(pending['cursor'], loop.time() + timeout)
        pending.setdefault('late', []).extend(line.strip() for line in body if line.strip())
        pending['body'] = pending['body'] + body
        if final is None:
            return None
        self._pending = None
        previous = {'command': pending['command'], 'late_response': pending['late'], 'result': final.strip()}
        # The CRC covers the whole response, the part the timed-out call got too.
        crc_ok = self._crc_ok(pending['body'], final, None)
        if crc_ok is not None:
            previous['crc_ok'] = crc_ok
        return previous

    @staticmethod
    def _crc_ok(body, final, crc_line):
        """The final line's CRC covers the whole response; None when there is none."""
        # crc_line is an empty Line, falsy: compare with None.
        crc = final.crc if final.crc is not None else (crc_line.crc if crc_line is not None else None)
        if crc is None:
            return None
        data = ''.join(line.wire + '\r\n' for line in body) + final.wire
        if crc_line is not None:
            data += '\r\n'
        return zlib.crc32(data.encode('utf-8')) == crc

    async def _wait_boot_delay(self):
        if self._boot_time is not None:
            wait = self._boot_time + _LOGIN_BOOT_DELAY - asyncio.get_running_loop().time()
            if wait > 0:
                await asyncio.sleep(wait)

    def _schedule_login(self):
        """Log in ahead of the first protected command, so `auth` says where
        things stand instead of waiting for a "Permission denied"."""
        if not self.passphrase or self._login_failed:
            return
        if self._login_task is not None and not self._login_task.done():
            return
        self._login_task = asyncio.get_running_loop().create_task(self._auto_login())

    async def _auto_login(self):
        # The boot delay passes outside the lock, AT commands go on meanwhile.
        await self._wait_boot_delay()
        async with self._at_lock:
            if not self._login_failed and self.connector.state == 'connected':
                await self._login()

    async def _login(self):
        await self._wait_boot_delay()
        for attempt in range(2):
            # The passphrase never goes into the terminal buffer, the console or its log file.
            r = await self._transact(f'AT$LOGIN="{self.passphrase}"', 5.0, echo='AT$LOGIN="***"')
            final = r['final'].strip() if r['status'] == 'done' and r['final'] is not None else ''
            # Refused during the boot delay without counting as an attempt;
            # the console may have come up after the @BOOT went by.
            if 'Login not available yet' not in final or attempt:
                break
            await asyncio.sleep(_LOGIN_BOOT_DELAY)
        ok = final == _RESULT_OK
        self._auth = 'logged_in' if ok else 'login_failed'
        # One failed attempt per boot at most: repeated ones run into the
        # firmware's login cooldown.
        self._login_failed = not ok
        logger.info(f'ATCI login: {self._auth}')
        return ok

    async def _at(self, command: str, timeout: float, raw: bool, force: bool = False) -> dict:
        async with self._at_lock:
            previous = None
            if self._pending is not None:
                # Whatever the device prints now still belongs to the command
                # that timed out; sending now would take its result for ours.
                previous = await self._finish_pending(timeout)
                if previous is None:
                    if not force:
                        return {'status': 'error', 'command': command, 'pending': self._pending['command'],
                                'error': f'the previous command {self._pending["command"]} timed out and has not '
                                         'finished yet; the device runs one command at a time. Retry later (its '
                                         'late response comes back in `previous`), or pass force=true to send anyway'}
                    self._pending = None
            urc_cursor = self._urc.total
            log_cursor = self._log.total
            r = await self._transact(command, timeout)
            if r['status'] == 'done' and r['final'] is not None and r['final'].strip() == _PERMISSION_DENIED:
                if self.passphrase and not self._login_failed and await self._login():
                    r = await self._transact(command, timeout)
                elif not self.passphrase:
                    self._auth = 'denied'
            if r['status'] == 'error':
                return {'status': 'error', 'command': command, 'error': r['error']}
            body, final = r['body'], r['final']
            response = [line.raw for line in body if line.strip()] if raw else [line.strip() for line in body if line.strip()]
            if final is None:
                status, result, crc_ok = 'timeout', '', None
            else:
                status = 'ok' if final.strip() == _RESULT_OK else 'error'
                result = final.raw if raw else final.strip()
                crc_ok = self._crc_ok(body, final, r['crc_line'])
            out = {'status': status, 'command': command, 'response': response, 'result': result,
                   'urc_cursor': urc_cursor, 'log_cursor': log_cursor}
            if crc_ok is not None:
                out['crc_ok'] = crc_ok
            if status == 'error' and result.strip() == _PERMISSION_DENIED:
                out['auth'] = self._auth
            if previous is not None:
                out['previous'] = previous
            return out

    def _register_tools(self):
        server = self
        annotations = self._annotations
        read = annotations(read_only_hint=True, open_world_hint=False)
        talk = annotations(read_only_hint=False, open_world_hint=False)
        control = annotations(read_only_hint=False, destructive_hint=False, open_world_hint=False)

        # Every tool is async: the SDK runs sync ones on a worker thread,
        # and the buffers are only ever touched on the event loop.
        @self._mcp.tool(annotations=talk)
        async def at_command(command: str, timeout: float = 5.0, raw: bool = False, force: bool = False) -> dict:
            """Send one AT command and wait for its final result ("OK" or "ERROR...").

            `response` holds the lines the device printed before the result,
            without URCs and logs (they go to `read_urc` / `read_log`). On a
            timeout the lines received so far are still returned, so a long
            command (e.g. AT$MBUSREAD) can be given a larger `timeout`.
            With `raw` the lines come verbatim, CRC ("\\t<CRC32>") included.
            In CRC mode `crc_ok` tells whether the final line's CRC, which
            covers the whole response, matches.
            Access to commands that need a login is handled by the server
            (when it has a passphrase); an "ERROR: \\"Permission denied\\""
            result comes with the `auth` state.

            After a timeout the device still runs the command (it takes one
            at a time), so the next call first waits, up to its own `timeout`,
            for that result and returns it in `previous`; while it is still
            running the call fails without sending. `force` sends anyway, at
            the risk of the late result being taken for this command's.

            Returns status "ok", "error" (see `result`) or "timeout", plus
            `urc_cursor` / `log_cursor` for what arrives after this command.
            """
            return await server._at(command, timeout, raw, force)

        @self._mcp.tool(annotations=talk)
        async def at_batch(commands: list[str | dict], stop_on_error: bool = True, timeout: float = 5.0,
                           raw: bool = False, force: bool = False) -> dict:
            """Run AT commands one after another, each like `at_command`.

            Each item is a command string, which gets `timeout`, or
            {"command": ..., "timeout": seconds} for one that needs longer,
            e.g. ["AT$CONFIG?", {"command": "AT$MBUSREAD=...", "timeout": 120}].
            With `stop_on_error` the batch stops at the first command that
            does not end with "OK".

            Returns `results` (one per command run), `failed` (how many did
            not return OK) and `status`: "ok" (all ran, all OK), "partial" (all
            ran, some failed) or "error" (stopped early).
            """
            items = []
            for item in commands:
                if isinstance(item, str):
                    items.append((item, timeout))
                elif isinstance(item, dict) and isinstance(item.get('command'), str):
                    items.append((item['command'], float(item.get('timeout', timeout))))
                else:
                    return {'status': 'error', 'error': f'bad batch item {item!r}: a command string or '
                                                        '{"command": ..., "timeout": seconds}'}
            results = []
            for command, command_timeout in items:
                result = await server._at(command, command_timeout, raw, force)
                results.append(result)
                if stop_on_error and result['status'] != 'ok':
                    break
            failed = sum(1 for r in results if r['status'] != 'ok')
            if len(results) < len(commands):
                status = 'error'
            else:
                status = 'partial' if failed else 'ok'
            return {'status': status, 'completed': len(results), 'failed': failed, 'results': results}

        @self._mcp.tool(annotations=read)
        async def read_urc(after_cursor: int = -1, lines: int = -1, pattern: str = '', raw: bool = False) -> dict:
            """Read buffered URCs (unsolicited "@..." lines such as @BOOT).

            Args:
                after_cursor: Only URCs after this cursor (`urc_cursor` of an
                    AT result, or `cursor` of an earlier call).
                lines: Number of most recent URCs (-1 = all buffered); with
                    `after_cursor` at most this many, oldest first.
                pattern: Regex filter (case-insensitive).
                raw: Lines verbatim, CRC included.

            Returns `urc` and `cursor` to pass as `after_cursor` next time
            (with a `lines` limit it points right after the last URC returned).
            URCs whose own CRC does not match are listed in `crc_failed`.
            """
            try:
                urcs, cursor = server._urc.read(after_cursor, lines, pattern)
            except re.error as e:
                return {'status': 'error', 'error': f'bad pattern: {e}'}
            out = {'status': 'ok', 'urc': [u.raw if raw else str(u) for u in urcs], 'cursor': cursor}
            failed = [str(u) for u in urcs if u.crc_ok() is False]
            if failed:
                out['crc_failed'] = failed
            return out

        @self._mcp.tool(annotations=read)
        async def read_log(after_cursor: int = -1, lines: int = -1, pattern: str = '') -> dict:
            """Read buffered device logs (@LOG lines), one entry per line.

            Args:
                after_cursor: Only logs after this cursor (`log_cursor` of an
                    AT result, or `cursor` of an earlier call).
                lines: Number of most recent lines (-1 = all buffered); with
                    `after_cursor` at most this many, oldest first.
                pattern: Regex filter (case-insensitive).

            Returns `log` and `cursor` to pass as `after_cursor` next time
            (with a `lines` limit it points right after the last line returned).
            """
            try:
                logs, cursor = server._log.read(after_cursor, lines, pattern)
            except re.error as e:
                return {'status': 'error', 'error': f'bad pattern: {e}'}
            return {'status': 'ok', 'log': logs, 'cursor': cursor}

        @self._mcp.tool(annotations=read)
        async def read_terminal(lines: int = 100) -> dict:
            """Recent console traffic in both directions (sent commands and every
            device line that is not a log), oldest first."""
            return {'status': 'ok', 'terminal': server._terminal.read(lines=lines)[0]}

        @self._mcp.tool(annotations=read)
        async def wait_for(pattern: str, source: str = 'urc', timeout: float = 60.0, after_cursor: int = -1) -> dict:
            """Wait until a line matching the regex `pattern` (case-insensitive)
            arrives, e.g. "@BOOT" after AT$REBOOT.

            Args:
                source: "urc", "log" or "output" (every device line but logs).
                timeout: Seconds to wait.
                after_cursor: Also match lines already buffered after this
                    cursor of `source` (e.g. `urc_cursor` of the AT$REBOOT
                    result), so a line that came before this call is not
                    missed. -1 = only lines arriving from now on.

            Returns status "ok" with the matching `line`, or "timeout"; both
            carry `cursor`.
            """
            buffers = {'urc': server._urc, 'log': server._log, 'output': server._output}
            if source not in buffers:
                return {'status': 'error', 'error': 'source must be "urc", "log" or "output"'}
            try:
                regex = re.compile(pattern, re.IGNORECASE)
            except re.error as e:
                return {'status': 'error', 'error': f'bad pattern: {e}'}
            buffer = buffers[source]
            cursor = after_cursor if after_cursor >= 0 else buffer.total
            deadline = asyncio.get_running_loop().time() + timeout
            while True:
                for line in buffer.since(cursor):
                    cursor += 1
                    if regex.search(line):
                        return {'status': 'ok', 'line': str(line), 'cursor': cursor}
                cursor = max(cursor, buffer.total - len(buffer.lines))
                if not await server._wait_new_line(deadline):
                    return {'status': 'timeout', 'cursor': cursor}

        @self._mcp.tool(annotations=read)
        async def status() -> dict:
            """Serial port and login state, and buffer cursors.

            `port.state`: "connected"; "disconnected" (the USB serial device
            went away, it is reopened as soon as it is back); "released"
            (handed over by `port_release`, call `port_acquire`).
            `auth`: "unknown", "logged_in", "login_failed" (not retried until
            the next @BOOT) or "denied" (a command needs a login and the
            server has no passphrase). With a passphrase the server logs in
            on its own when the port opens and after every @BOOT.
            """
            conn = server.connector
            return {
                'status': 'ok',
                'port': {'device': conn.port, 'baudrate': conn.baudrate, 'state': conn.state, 'error': conn.error},
                'auth': server._auth,
                'login_configured': bool(server.passphrase),
                'cursors': {'urc': server._urc.total, 'log': server._log.total, 'output': server._output.total},
            }

        @self._mcp.tool(annotations=control)
        async def port_release() -> dict:
            """Close the serial port so another tool (a flasher, a test script,
            the service app) can open it. It stays closed until `port_acquire`."""
            if server._at_lock.locked():
                return {'status': 'error', 'error': 'an AT command is running, retry when it is done'}
            await asyncio.get_running_loop().run_in_executor(None, server.connector.release)
            return {'status': 'ok', 'state': server.connector.state}

        @self._mcp.tool(annotations=control)
        async def port_acquire() -> dict:
            """Open the serial port again after `port_release`. Fails while
            another tool still holds it."""
            try:
                await asyncio.get_running_loop().run_in_executor(None, server.connector.acquire)
            except (serial.SerialException, OSError) as e:
                return {'status': 'error', 'error': str(e)}
            return {'status': 'ok', 'state': server.connector.state}


class SerialConsole(WestCommand):
    def __init__(self):
        super().__init__(
            'serial-console',
            'start a serial console',
            'This command starts a serial console on the specified port.')

    def do_add_parser(self, parser_adder):
        parser = parser_adder.add_parser(self.name,
                                         help=self.help,
                                         description=self.description)
        parser.add_argument('--port', type=str, default='/dev/ttyUSB0',
                            help='Serial port to connect to (a /dev/serial/by-id/ path survives replugging), '
                                 'or file:PATH to read from a file')
        parser.add_argument('--baudrate', type=int, default=1000000,  # 115200,
                            help='Baud rate for the serial connection')
        parser.add_argument('--history-file', type=str, default=DEFAULT_HISTORY_FILE,
                            help='File to store command history')
        parser.add_argument('--console-file', type=str, default=DEFAULT_CONSOLE_FILE,
                            help='File to store console output')
        parser.add_argument('--modem-trace-file', type=str, default=DEFAULT_MODEM_TRACE_FILE,
                            help='File to store modem trace output')
        parser.add_argument('--input', type=str, default='',
                            help='Initial input to send to the console. Can be a string or a path to a file whose contents will be sent.')
        parser.add_argument('--wake-idle', type=float, default=2.0,
                            help='Send a wake-up ESC ahead of a command after this many idle seconds (0 = never)')
        parser.add_argument('--mcp', action=argparse.BooleanOptionalAction, default=True,
                            help='Run an MCP server for AI agents (AT commands, URCs, logs) on a serial port')
        parser.add_argument('--mcp-listen', type=str, default=DEFAULT_MCP_LISTEN,
                            help='MCP server listen address [host:]port')
        parser.add_argument('--mcp-token', type=str, default=None,
                            help='Require "Authorization: Bearer TOKEN" on the MCP server')
        parser.add_argument('--passphrase', type=str, default=os.environ.get(PASSPHRASE_ENV),
                            help=f'ATCI passphrase the MCP server logs in with when a command is denied '
                                 f'(default: ${PASSPHRASE_ENV})')
        return parser

    def do_run(self, args, unknown_args):
        logger.remove()

        port = args.port

        if port.startswith('file:'):
            port = os.path.expanduser(port[5:])  # remove 'file:' prefix and expand ~
            connector = FileConnector(filename=port)
            connector.set_modem_trace_file(args.modem_trace_file)
            source_text = f'File: {port}'
        else:
            connector = SerialConnector(port=port, baudrate=args.baudrate, wake_idle=args.wake_idle)
            connector.set_modem_trace_file(args.modem_trace_file)
            source_text = f'Device: {port}'

            if args.mcp:
                if args.passphrase and '"' in args.passphrase:
                    sys.exit('The passphrase must not contain a double quote: AT$LOGIN="..." cannot carry it')
                try:
                    connector = SerialMCPServer(connector, listen=args.mcp_listen, token=args.mcp_token,
                                                passphrase=args.passphrase)
                except MCPPortInUseError as e:
                    sys.exit(f'MCP port {e} is already in use (another serial console?), '
                             'pick another with --mcp-listen or disable the server with --no-mcp')

            if args.input:
                connector = InputMiddleware(connector, args.input)

        if args.console_file:
            connector = FileLogConnector(connector, args.console_file, text=source_text)

        console = Console(connector=connector, history_file=args.history_file)
        console.run()


def main():
    """Standalone entry point for running without west."""
    cmd = SerialConsole()

    # Create a mock parser_adder that returns ArgumentParser directly
    class ParserAdder:
        def add_parser(self, name, help, description):
            return argparse.ArgumentParser(prog=name, description=description)

    parser = cmd.do_add_parser(ParserAdder())
    args = parser.parse_args()

    cmd.do_run(args, [])


if __name__ == '__main__':
    main()
