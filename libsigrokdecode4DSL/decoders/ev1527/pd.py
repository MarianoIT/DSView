##
## This file is part of the DSView project.
##
## Copyright (C) 2026 DreamSourceLab <support@dreamsourcelab.com>
##
## This program is free software: you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation, either version 3 of the License, or
## (at your option) any later version.
##

import sigrokdecode as srd


class Decoder(srd.Decoder):
    api_version = 3
    id = 'ev1527'
    name = 'EV1527'
    longname = 'EV1527 remote control encoder'
    desc = 'Decode EV1527 frames: 1T/31T preamble, 1T/3T=0, 3T/1T=1.'
    license = 'gplv3+'
    inputs = ['logic']
    outputs = []
    tags = ['IC', 'RF']
    channels = (
        {'id': 'data', 'name': 'Data', 'desc': 'Demodulated OOK data signal'},
    )
    options = (
        {'id': 'tolerance', 'desc': 'Pulse tolerance', 'default': '35',
         'values': ('20', '25', '30', '35', '40', '50')},
    )
    annotations = (
        ('bit-0', 'Bit 0'),
        ('bit-1', 'Bit 1'),
        ('sync', 'Sync'),
        ('frame', 'Frame'),
        ('error', 'Error'),
    )
    annotation_rows = (
        ('bits', 'Bits', (0, 1, 2, 4)),
        ('frames', 'Frames', (3,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.last_edge = None
        self.unit = None
        self.bit_pulses = []
        self.bits = []
        self.frame_start = None
        self.frame_count = 0

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.tolerance = int(self.options['tolerance']) / 100.0

    def is_short(self, width):
        return abs(width - self.unit) <= self.unit * self.tolerance

    def is_long(self, width):
        target = self.unit * 3
        return abs(width - target) <= target * self.tolerance

    def is_preamble_gap(self, width):
        target = self.unit * 31
        return abs(width - target) <= target * self.tolerance

    def reset_frame(self):
        self.bit_pulses = []
        self.bits = []
        self.frame_start = None
        self.unit = None

    def frame_name(self):
        name = ''
        index = self.frame_count
        while True:
            name = chr(ord('A') + index % 26) + name
            index = index // 26 - 1
            if index < 0:
                return name

    @staticmethod
    def format_bits(bits):
        return ' '.join(bits[index:index + 4] for index in range(0, len(bits), 4))

    def emit_frame(self, end):
        if self.bits:
            message = 'Frame %s: %s' % (self.frame_name(), self.format_bits(self.bits))
            self.put(self.frame_start, end, self.out_ann, [3, [message]])
            self.frame_count += 1
        self.reset_frame()

    def decode_bit(self):
        start = self.bit_pulses[0][0]
        end = self.bit_pulses[1][1]
        first = self.bit_pulses[0][1] - self.bit_pulses[0][0]
        second = self.bit_pulses[1][1] - self.bit_pulses[1][0]

        if self.is_short(first) and self.is_long(second):
            value = '0'
        elif self.is_long(first) and self.is_short(second):
            value = '1'
        else:
            self.put(start, end, self.out_ann, [4, ['Invalid EV1527 pulse']])
            self.reset_frame()
            return

        self.put(start, end, self.out_ann, [int(value), [value]])
        self.bits.append(value)

    def decode(self):
        while True:
            self.wait({0: 'e'})
            current_edge = self.samplenum

            if self.last_edge is None:
                self.last_edge = current_edge
                continue

            width = current_edge - self.last_edge
            pulse_start = self.last_edge
            self.last_edge = current_edge

            if self.unit is None:
                self.unit = width
                continue

            if self.frame_start is None:
                if self.is_preamble_gap(width):
                    self.frame_start = current_edge
                    self.put(pulse_start - self.unit, current_edge, self.out_ann,
                             [2, ['Preamble']])
                    continue

                self.reset_frame()
                self.unit = width
                continue

            self.bit_pulses.append((pulse_start, current_edge))
            if len(self.bit_pulses) == 2:
                self.decode_bit()
                self.bit_pulses = []
