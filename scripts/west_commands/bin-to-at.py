import os
import base64
import glob

from west import log
from west.commands import WestCommand

class BinToAt(WestCommand):
    def __init__(self):
        super().__init__(
            'bin-to-at',
            'convert binary file to AT commands',
            'This command converts a binary file to AT commands.')

    def do_add_parser(self, parser_adder):
        parser = parser_adder.add_parser(self.name,
                                         help=self.help,
                                         description=self.description)
        parser.add_argument('--output-file', type=str, default='output.at',
                            help='File to store AT commands output')
        parser.add_argument('--input-file', type=str,
                            help='Binary file to convert to AT commands')
        return parser

    def do_run(self, args, unknown_args):
        if not args.input_file:
            ff = glob.glob(os.path.join('.', 'build', '*', 'zephyr', 'zephyr.signed.bin'))
            if ff:
                args.input_file = ff[0]
            else:
                log.die("No input file specified and no default binary found.")

        if not os.path.isfile(args.input_file):
            log.die(f"Input file '{args.input_file}' does not exist.")

        out = open(args.output_file, 'w')
        out.write('AT$FW?\n')
        out.write('AT$FW="info"\n')
        chunk_size = 64

        with open(args.input_file, 'rb') as f:
            data = f.read()
            out.write(f'AT$FW="start",{len(data)}\n')
            for offset in range(0, len(data), chunk_size):
                chunk = data[offset:offset + chunk_size]
                # encoded_chunk = base64.b64encode(chunk).decode('utf-8')
                encoded_chunk = ''.join(f'{byte:02x}' for byte in chunk)
                out.write(f'AT$FW="chunk",{offset},"{encoded_chunk}"\n')
            out.write('AT$FW="done"\n')

        out.write('AT$FW="info"\n')
