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


class SamplerateError(Exception):
    pass


class Decoder(srd.Decoder):
    api_version = 3
    id = 'clock'
    name = 'Clock'
    longname = 'Clock frequency'
    desc = 'Measure the frequency of a periodic digital clock signal.'
    license = 'gplv3+'
    inputs = ['logic']
    outputs = []
    tags = ['Clock/timing', 'Util']
    channels = (
        {'id': 'clock', 'name': 'Clock', 'desc': 'Clock signal'},
    )
    options = (
        {'id': 'edge', 'desc': 'Measurement edge', 'default': 'rising',
         'values': ('rising', 'falling')},
    )
    annotations = (
        ('frequency', 'Frequency'),
        ('period', 'Period'),
    )
    annotation_rows = (
        ('frequency', 'Clock frequency', (0, 1)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.last_edge = None

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    @staticmethod
    def format_frequency(frequency):
        if frequency >= 1e9:
            return '%.3f GHz' % (frequency / 1e9)
        if frequency >= 1e6:
            return '%.3f MHz' % (frequency / 1e6)
        if frequency >= 1e3:
            return '%.3f kHz' % (frequency / 1e3)
        return '%.3f Hz' % frequency

    @staticmethod
    def format_period(period):
        if period >= 1:
            return '%.3f s' % period
        if period >= 1e-3:
            return '%.3f ms' % (period * 1e3)
        if period >= 1e-6:
            return '%.3f us' % (period * 1e6)
        if period >= 1e-9:
            return '%.3f ns' % (period * 1e9)
        return '%.3f ps' % (period * 1e12)

    def decode(self):
        if not self.samplerate:
            raise SamplerateError('Cannot decode without samplerate.')

        edge = 'r' if self.options['edge'] == 'rising' else 'f'
        self.wait({0: edge})
        self.last_edge = self.samplenum

        while True:
            self.wait({0: edge})
            period_samples = self.samplenum - self.last_edge
            if period_samples > 0:
                frequency = float(self.samplerate) / float(period_samples)
                period = float(period_samples) / float(self.samplerate)
                self.put(self.last_edge, self.samplenum, self.out_ann,
                         [0, ['Frequency: %s' % self.format_frequency(frequency),
                              self.format_frequency(frequency)]])
                self.put(self.last_edge, self.samplenum, self.out_ann,
                         [1, ['Period: %s' % self.format_period(period),
                              self.format_period(period)]])
            self.last_edge = self.samplenum
