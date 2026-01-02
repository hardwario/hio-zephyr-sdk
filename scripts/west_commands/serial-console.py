#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
import serial
import threading
import time
import os
import base64
import queue
from loguru import logger
from rttt.console import Console
from rttt.connectors import FileLogConnector
from rttt.connectors.base import Connector
from rttt.event import Event, EventType

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
                line = self.lines.get(timeout=1)

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
                    self._emit(Event(EventType.OUT, line.encode('utf-8').decode('unicode_escape')))
            except queue.Empty:
                continue
            except Exception as e:
                logger.error(f"Error processing line: {e}")


class SerialConnector(BaseConsoleConnector):

    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 0.2) -> None:
        super().__init__()
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser = None
        self._cache = ''

    def open(self):
        logger.info(f"Opening serial port {self.port} at {self.baudrate} baud")
        self.ser = serial.Serial(self.port, self.baudrate, timeout=self.timeout)
        self._start_threads()
        self._emit(Event(EventType.OPEN, ''))
        logger.info("Serial connection opened")

    def _close_resources(self):
        if self.ser and self.ser.is_open:
            self.ser.close()

    def handle(self, event: Event):
        logger.info(f'handle: {event.type} {event.data}')
        if event.type == EventType.IN:
            data = f'\x1b{event.data}\n'.encode('utf-8')
            self.ser.write(data)
        self._emit(event)

    def _read_task(self):
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
        while self.is_running:
            try:
                if self.ser.in_waiting:
                    data = self.ser.read(self.ser.in_waiting)
                    if data:
                        text = data.decode('utf-8', errors='backslashreplace')
                        text = self._cache + text
                        while True:
                            newline_index = text.find('\n')
                            if newline_index == -1:
                                self._cache = text
                                break

                            line = text[:newline_index].rstrip('\r')
                            text = text[newline_index + 1:]

                            if len(line) > 10 and line[-9] == '\t':
                                line = line[:-9]  # remove CRC

                            self.lines.put(line)

            except Exception as e:
                logger.warning(f"Serial read error: {e}")


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


def input_task(connector: Connector, arg: str):
    logger.info(f"Initial input: {arg}")
    signal = threading.Event()

    is_file = os.path.exists(arg)

    def task():
        time.sleep(0.5)
        if is_file:
            logger.info(f'Starting input task for file: {arg}')
            try:
                with open(arg, 'r', encoding='utf-8') as f:
                    for line in f:
                        line = line.strip()
                        if line:
                            logger.info(f'Sending line: {line}')
                            signal.clear()
                            connector.handle(Event(EventType.IN, line))
                            if not signal.wait(timeout=1):
                                logger.warning("Timeout waiting for OK")
                            # time.sleep(0.01)  # optional
                logger.info('Done input task from file')
            except Exception as e:
                logger.error(f"Failed to read file {arg}: {e}")
        else:
            logger.info('Starting input task')
            time.sleep(0.5)
            connector.handle(Event(EventType.IN, arg))
            logger.info('Done input task')

    def handler(event: Event):
        if event.type == EventType.OPEN:
            threading.Thread(target=task, daemon=True).start()
        elif event.type == EventType.OUT and event.data == 'OK':
            signal.set()

    connector.on(handler)


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
                            help='Serial port to connect to, or file:PATH to read from a file')
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
            connector = SerialConnector(port=port, baudrate=args.baudrate)
            connector.set_modem_trace_file(args.modem_trace_file)
            source_text = f'Device: {port}'

            if args.input:
                input_task(connector, args.input)

        if args.console_file:
            connector = FileLogConnector(connector, args.console_file, text=source_text)

        console = Console(connector=connector, history_file=args.history_file)
        console.run()


def main():
    """Standalone entry point for running without west."""
    import argparse

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
