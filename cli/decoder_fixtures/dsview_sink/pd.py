"""Stacked decoder must receive the parent flush before its own end hook."""
import sigrokdecode as srd
class Decoder(srd.Decoder):
    api_version = 3
    id = 'dsview_sink'
    name = longname = desc = 'Stack completion regression'
    license = 'gplv3+'
    inputs = ['dsview_regression']
    outputs = []
    annotations = (('109', 'done', 'Done'),)
    def reset(self):
        pass
    def start(self):
        self.value = None
        self.ann = self.register(srd.OUTPUT_ANN)
    def decode(self, ss, es, data):
        self.value = data
    def end(self):
        if self.value != 0xa5:
            raise RuntimeError('Parent completion data missing')
        self.put(0, self.last_samplenum, self.ann, [0, ['negative', -1]])
