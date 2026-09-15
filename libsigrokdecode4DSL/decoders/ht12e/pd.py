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
    id = 'ht12e'
    name = 'HT12E'
    longname = 'HT12E remote control encoder'
    desc = 'Decode HT12E pulse frames with 001=1 and 011=0.'
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
        self.pulses = []
        self.bits = []
        self.frame_start = None
        self.unit = None
        self.awaiting_frame = True
        self.frame_count = 0

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.tolerance = int(self.options['tolerance']) / 100.0

    def is_short(self, width):
        return self.unit and abs(width - self.unit) <= self.unit * self.tolerance

    def is_long(self, width):
        return self.unit and abs(width - self.unit * 3) <= self.unit * 3 * self.tolerance

    def reset_frame(self):
        self.pulses = []
        self.bits = []
        self.frame_start = None
        self.awaiting_frame = True
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

    def finish_frame(self, end):
        if self.bits:
            message = 'Frame %s: %s' % (self.frame_name(), self.format_bits(self.bits))
            self.put(self.frame_start, end, self.out_ann, [3, [message]])
            self.frame_count += 1
        self.reset_frame()

    def emit_bit(self, value, start, end):
        annotation = 1 if value == '1' else 0
        self.put(start, end, self.out_ann, [annotation, [value]])
        self.bits.append(value)

    def decode_symbol(self):
        start = self.pulses[0][0]
        end = self.pulses[-1][1]
        widths = [pulse[1] - pulse[0] for pulse in self.pulses]

        if all(self.is_short(width) for width in widths[:2]) and self.is_long(widths[2]):
            self.emit_bit('1', start, end)
        elif self.is_short(widths[0]) and all(self.is_long(width) for width in widths[1:]):
            self.emit_bit('0', start, end)
        else:
            self.put(start, end, self.out_ann, [4, ['Invalid pulse']])
            self.reset_frame()

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

            if self.unit and width >= self.unit * 10:
                self.finish_frame(pulse_start)
                self.put(pulse_start, current_edge, self.out_ann, [2, ['Sync']])
                continue

            if self.awaiting_frame:
                self.unit = width
                self.frame_start = pulse_start
                self.awaiting_frame = False
                continue

            self.pulses.append((pulse_start, current_edge))
            if len(self.pulses) == 3:
                self.decode_symbol()
                self.pulses = []