import serial
import threading
import time
import os
import base64
import queue

from west import log
from west.commands import WestCommand

from loguru import logger
from rttt.console import Console
from rttt.connectors import FileLogConnector
from rttt.connectors.base import Connector
from rttt.event import Event, EventType

DEFAULT_HISTORY_FILE = os.path.expanduser(f"~/.serial_console_history")
DEFAULT_CONSOLE_FILE = os.path.expanduser(f"~/.serial_console_console")
DEFAULT_MODEM_TRACE_FILE = os.path.expanduser(f"~/.serial_console.mtrace")


class SerialConnector(Connector):

    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 0.2) -> None:
        super().__init__()
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser = None
        self._thread_read = None
        self._thread_line = None
        self.is_running = False
        self._cache = ''
        self.lines = queue.Queue()
        self.modem_trace_fd = None

    def set_modem_trace_file(self, modem_trace_file: str):
        """
        Set the file where modem trace data will be written.
        This method is called by the Console to set the modem trace file.
        """
        logger.info(f"Modem trace file set to: {modem_trace_file}")
        self.modem_trace_fd = open(modem_trace_file, 'wb')

    def open(self):
        logger.info(f"Opening serial port {self.port} at {self.baudrate} baud")
        self.ser = serial.Serial(self.port, self.baudrate, timeout=self.timeout)
        self.is_running = True
        self._thread_read = threading.Thread(target=self._read_task, daemon=True)
        self._thread_read.start()
        self._thread_line = threading.Thread(target=self._line_task, daemon=True)
        self._thread_line.start()

        self._emit(Event(EventType.OPEN, ''))
        logger.info("Serial connection opened")

    def close(self):
        logger.info("Closing serial port")
        if not self.is_running:
            return
        self.is_running = False
        if self._thread_read:
            self._thread_read.join()
        if self._thread_line:
            self._thread_line.join()
        if self.ser and self.ser.is_open:
            self.ser.close()
        if self.modem_trace_fd:
            self.modem_trace_fd.close()
        self._emit(Event(EventType.CLOSE, ''))
        logger.info("Serial connection closed")

    def handle(self, event: Event):
        logger.info(f'handle: {event.type} {event.data}')
        if event.type == EventType.IN:
            data = f'{event.data}\n'.encode('utf-8')
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
                                line = line[:-9] # remove CRC

                            self.lines.put(line)

            except Exception as e:
                logger.warning(f"Serial read error: {e}")

    def _line_task(self):
        while self.is_running:
            try:
                line = self.lines.get(timeout=1)

                if line.startswith("@MT: "):
                    if self.modem_trace_fd:
                        index = line.find(',')
                        b64text = line[index + 2:-1]
                        if b64text:
                            data = base64.b64decode(b64text)
                            self.modem_trace_fd.write(data)
                elif line.startswith("@LOG: "):
                    self._emit(Event(EventType.LOG, line[7:-1].encode('utf-8').decode('unicode_escape')))
                else:
                    self._emit(Event(EventType.OUT, line))
            except queue.Empty:
                continue
            except Exception as e:
                logger.error(f"Error processing modem trace: {e}")


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
                            help='Serial port to connect to')
        parser.add_argument('--baudrate', type=int, default=1000000,# 115200,
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

        connector = SerialConnector(port=args.port, baudrate=args.baudrate)
        connector.set_modem_trace_file(args.modem_trace_file)

        if args.input:
            input_task(connector, args.input)

        if args.console_file:
            text = f'Device: {args.port}'
            connector = FileLogConnector(connector, args.console_file, text=text)

        console = Console(connector=connector, history_file=args.history_file)
        console.run()
