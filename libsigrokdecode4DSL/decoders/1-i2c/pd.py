##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) 2010-2016 Uwe Hermann <uwe@hermann-uwe.de>
## Copyright (C) 2022 DreamSourceLab <support@dreamsourcelab.com>
##
## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 2 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program; if not, see <http://www.gnu.org/licenses/>.
##

# Passive captures cannot identify which controller loses multi-master
# arbitration, but malformed partial bytes are reported as bus warnings.

##
## 2022/07/05 DreamSourceLab : Support for different data output formats
##

import sigrokdecode as srd
from common.sigrok_compat import match_mask

'''
OUTPUT_PYTHON format:

Packet:
[<ptype>, <pdata>]

<ptype>:
 - 'START' (START condition)
 - 'START REPEAT' (Repeated START condition)
 - 'ADDRESS READ' (Slave address, read)
 - 'ADDRESS WRITE' (Slave address, write)
 - 'DATA READ' (Data, read)
 - 'DATA WRITE' (Data, write)
 - 'STOP' (STOP condition)
 - 'ACK' (ACK bit)
 - 'NACK' (NACK bit)
 - 'BITS' (<pdata>: list of data/address bits and their ss/es numbers)

<pdata> is the data or address byte associated with the 'ADDRESS*' and 'DATA*'
command. Slave addresses do not include bit 0 (the READ/WRITE indication bit).
For example, a slave address field could be 0x51 (instead of 0xa2).
For 'START', 'START REPEAT', 'STOP', 'ACK', and 'NACK' <pdata> is None.
'''

# CMD: [annotation-type-index, long annotation, short annotation]
proto = {
    'START':           [0, 'Start',         'S'],
    'START REPEAT':    [1, 'Start repeat',  'Sr'],
    'STOP':            [2, 'Stop',          'P'],
    'ACK':             [3, 'ACK',           'A'],
    'NACK':            [4, 'NACK',          'N'],
    'BIT':             [5, 'Bit',           'B'],
    'ADDRESS READ':    [6, 'Address read',  'AR'],
    'ADDRESS WRITE':   [7, 'Address write', 'AW'],
    'DATA READ':       [8, 'Data read',     'DR'],
    'DATA WRITE':      [9, 'Data write',    'DW'],
}

class Decoder(srd.Decoder):
    api_version = 3
    id = '1:i2c'
    name = '1:I²C'
    longname = 'Inter-Integrated Circuit'
    desc = 'Two-wire, multi-master, serial bus.'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = ['i2c']
    tags = ['Embedded/industrial']
    channels = (
        {'id': 'scl', 'type': 8, 'name': 'SCL', 'desc': 'Serial clock line', 'idn':'dec_1i2c_chan_scl'},
        {'id': 'sda', 'type': 108, 'name': 'SDA', 'desc': 'Serial data line', 'idn':'dec_1i2c_chan_sda'},
    )
    options = (
        {'id': 'address_format', 'desc': 'Displayed slave address format',
            'default': 'unshifted', 'values': ('shifted', 'unshifted'), 'idn':'dec_1i2c_opt_addr'},
        {'id': 'invert_scl', 'desc': 'Invert SCL', 'default': 'no',
            'values': ('no', 'yes')},
        {'id': 'invert_sda', 'desc': 'Invert SDA', 'default': 'no',
            'values': ('no', 'yes')},
    )
    annotations = (
        ('7', 'start', 'Start condition'),
        ('6', 'repeat-start', 'Repeat start condition'),
        ('1', 'stop', 'Stop condition'),
        ('5', 'ack', 'ACK'),
        ('0', 'nack', 'NACK'),
        ('208', 'bit', 'Data/address bit'),
        ('112', 'address-read', 'Address read'),
        ('111', 'address-write', 'Address write'),
        ('110', 'data-read', 'Data read'),
        ('109', 'data-write', 'Data write'),
        ('1000', 'warnings', 'Human-readable warnings'),
    )
    annotation_rows = (
        ('bits', 'Bits', (5,)),
        ('addr-data', 'Address/Data', (0, 1, 2, 3, 4, 6, 7, 8, 9)),
        ('warnings', 'Warnings', (10,)),
    )
    binary = (
        ('address-read', 'Address read'),
        ('address-write', 'Address write'),
        ('data-read', 'Data read'),
        ('data-write', 'Data write'),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.ss = self.es = self.ss_byte = -1
        self.bitcount = 0
        self.databyte = 0
        self.wr = -1
        self.is_repeat_start = 0
        self.state = 'FIND START'
        self.pdu_start = None
        self.pdu_bits = 0
        self.bits = []

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        self.out_python = self.register(srd.OUTPUT_PYTHON)
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.out_binary = self.register(srd.OUTPUT_BINARY)
        self.out_bitrate = self.register(srd.OUTPUT_META,
                meta=(int, 'Bitrate', 'Bitrate from Start bit to Stop bit'))
        self.invert_scl = self.options['invert_scl'] == 'yes'
        self.invert_sda = self.options['invert_sda'] == 'yes'
        self.scl_sample_edge = 'f' if self.invert_scl else 'r'
        self.scl_high_level = 'l' if self.invert_scl else 'h'
        self.sda_start_edge = 'r' if self.invert_sda else 'f'
        self.sda_stop_edge = 'f' if self.invert_sda else 'r'

    def putx(self, data):
        self.put(self.ss, self.es, self.out_ann, data)

    def putp(self, data):
        self.put(self.ss, self.es, self.out_python, data)

    def putb(self, data):
        self.put(self.ss, self.es, self.out_binary, data)

    def normalized_levels(self, scl, sda):
        return (int(scl) ^ self.invert_scl, int(sda) ^ self.invert_sda)

    def warn_partial_byte(self, condition):
        if self.bitcount:
            self.ss = self.es = self.samplenum
            self.putx([10, ['%s during incomplete byte' % condition, 'Byte error']])

    def handle_start(self):
        self.ss, self.es = self.samplenum, self.samplenum
        self.pdu_start = self.samplenum
        self.pdu_bits = 0
        cmd = 'START REPEAT' if (self.is_repeat_start == 1) else 'START'
        self.putp([cmd, None])
        self.putx([proto[cmd][0], proto[cmd][1:]])
        self.state = 'FIND ADDRESS'
        self.bitcount = self.databyte = 0
        self.is_repeat_start = 1
        self.wr = -1
        self.bits = []

    # Gather 8 bits of data plus the ACK/NACK bit.
    def handle_address_or_data(self, scl, sda):
        self.pdu_bits += 1

        # Address and data are transmitted MSB-first.
        self.databyte <<= 1
        self.databyte |= sda

        # Remember the start of the first data/address bit.
        if self.bitcount == 0:
            self.ss_byte = self.samplenum

        # Store individual bits and their start/end samplenumbers.
        # In the list, index 0 represents the LSB (I²C transmits MSB-first).
        self.bits.insert(0, [sda, self.samplenum, self.samplenum])
        if self.bitcount > 0:
            self.bits[1][2] = self.samplenum
        if self.bitcount == 7:
            self.bitwidth = self.bits[1][2] - self.bits[2][2]
            self.bits[0][2] += self.bitwidth

        # Return if we haven't collected all 8 + 1 bits, yet.
        if self.bitcount < 7:
            self.bitcount += 1
            return

        d = self.databyte
        if self.state == 'FIND ADDRESS':
            # The READ/WRITE bit is only in address bytes, not data bytes.
            self.wr = 0 if (self.databyte & 1) else 1
            if self.options['address_format'] == 'shifted':
                d = d >> 1

        bin_class = -1
        if self.state == 'FIND ADDRESS' and self.wr == 1:
            cmd = 'ADDRESS WRITE'
            bin_class = 1
        elif self.state == 'FIND ADDRESS' and self.wr == 0:
            cmd = 'ADDRESS READ'
            bin_class = 0
        elif self.state == 'FIND DATA' and self.wr == 1:
            cmd = 'DATA WRITE'
            bin_class = 3
        elif self.state == 'FIND DATA' and self.wr == 0:
            cmd = 'DATA READ'
            bin_class = 2

        self.ss, self.es = self.ss_byte, self.samplenum + self.bitwidth

        self.putp(['BITS', self.bits])
        self.putp([cmd, d])

        self.putb([bin_class, bytes([d])])

        for bit in self.bits:
            self.put(bit[1], bit[2], self.out_ann, [5, ['%d' % bit[0]]])

        if cmd.startswith('ADDRESS'):
            self.ss, self.es = self.samplenum, self.samplenum + self.bitwidth
            w = ['Write', 'Wr', 'W'] if self.wr else ['Read', 'Rd', 'R']
            self.putx([0, w])
            self.ss, self.es = self.ss_byte, self.samplenum

        self.putx([proto[cmd][0], ['%s: {$}' % proto[cmd][1], '%s: {$}' % proto[cmd][2], '{$}', d]])

        # Done with this packet.
        self.bitcount = self.databyte = 0
        self.bits = []
        self.state = 'FIND ACK'

    def get_ack(self, scl, sda):
        self.ss, self.es = self.samplenum, self.samplenum + self.bitwidth
        cmd = 'NACK' if (sda == 1) else 'ACK'
        self.putp([cmd, None])
        self.putx([proto[cmd][0], proto[cmd][1:]])
        # There could be multiple data bytes in a row, so either find
        # another data byte or a STOP condition next.
        self.state = 'FIND DATA'

    def handle_stop(self):
        # Meta bitrate
        if self.samplerate:
            elapsed = 1 / float(self.samplerate) * (self.samplenum - self.pdu_start + 1)
            bitrate = int(1 / elapsed * self.pdu_bits)
            self.put(self.ss_byte, self.samplenum, self.out_bitrate, bitrate)

        cmd = 'STOP'
        self.ss, self.es = self.samplenum, self.samplenum
        self.putp([cmd, None])
        self.putx([proto[cmd][0], proto[cmd][1:]])
        self.state = 'FIND START'
        self.is_repeat_start = 0
        self.wr = -1
        self.bits = []

    def decode(self):
        while True:
            # State machine.
            if self.state == 'FIND START':
                # Wait for a START condition (S): SCL = high, SDA = falling.
                self.wait({0: self.scl_high_level, 1: self.sda_start_edge})
                self.handle_start()
            elif self.state == 'FIND ADDRESS':
                # Wait for any of the following conditions (or combinations):
                #  a) Data sampling of receiver: SCL = rising, and/or
                #  b) START condition (S): SCL = high, SDA = falling, and/or
                #  c) STOP condition (P): SCL = high, SDA = rising
                (scl, sda) = self.wait([{0: self.scl_sample_edge},
                                        {0: self.scl_high_level, 1: self.sda_start_edge},
                                        {0: self.scl_high_level, 1: self.sda_stop_edge}])
                (scl, sda) = self.normalized_levels(scl, sda)

                # Check which of the condition(s) matched and handle them.
                if (match_mask(self.matched) & (0b1 << 0)):
                    self.handle_address_or_data(scl, sda)
                elif (match_mask(self.matched) & (0b1 << 1)):
                    self.warn_partial_byte('START')
                    self.handle_start()
                elif (match_mask(self.matched) & (0b1 << 2)):
                    self.warn_partial_byte('STOP')
                    self.handle_stop()
            elif self.state == 'FIND DATA':
                # Wait for any of the following conditions (or combinations):
                #  a) Data sampling of receiver: SCL = rising, and/or
                #  b) START condition (S): SCL = high, SDA = falling, and/or
                #  c) STOP condition (P): SCL = high, SDA = rising
                (scl, sda) = self.wait([{0: self.scl_sample_edge},
                                        {0: self.scl_high_level, 1: self.sda_start_edge},
                                        {0: self.scl_high_level, 1: self.sda_stop_edge}])
                (scl, sda) = self.normalized_levels(scl, sda)

                # Check which of the condition(s) matched and handle them.
                if (match_mask(self.matched) & (0b1 << 0)):
                    self.handle_address_or_data(scl, sda)
                elif (match_mask(self.matched) & (0b1 << 1)):
                    self.warn_partial_byte('START')
                    self.handle_start()
                elif (match_mask(self.matched) & (0b1 << 2)):
                    self.warn_partial_byte('STOP')
                    self.handle_stop()
            elif self.state == 'FIND ACK':
                # Wait for any of the following conditions (or combinations):
                #  a) a data/ack bit: SCL = rising.
                #  b) STOP condition (P): SCL = high, SDA = rising
                (scl, sda) = self.wait([{0: self.scl_sample_edge},
                                        {0: self.scl_high_level, 1: self.sda_stop_edge}])
                (scl, sda) = self.normalized_levels(scl, sda)
                if (match_mask(self.matched) & (0b1 << 0)):
                    self.get_ack(scl, sda)
                elif (match_mask(self.matched) & (0b1 << 1)):
                    self.handle_stop()

