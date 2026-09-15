"""Exercise real engine annotation conversion and completion ordering."""
import sigrokdecode as srd
class Decoder(srd.Decoder):
    api_version = 3
    id = 'dsview_regression'
    name = longname = desc = 'Completion regression'
    license = 'gplv3+'
    inputs = ['logic']
    outputs = ['dsview_regression']
    channels = ({'id': 'data', 'name': 'Data', 'desc': 'Data'},)
    annotations = (('108', 'done', 'Done'),)
    def reset(self):
        pass
    def start(self):
        self.ann = self.register(srd.OUTPUT_ANN)
        self.out = self.register(srd.OUTPUT_PYTHON)
    def decode(self):
        while True:
            self.wait()
    def end(self):
        self.put(0, self.last_samplenum, self.ann, [0, [str(i) for i in range(12)] + [0xa5]])
        self.put(0, self.last_samplenum, self.out, 0xa5)
