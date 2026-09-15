"""A Python failure must reach the session caller without hanging."""
import sigrokdecode as srd
class Decoder(srd.Decoder):
    api_version = 3
    id = 'dsview_error'
    name = longname = desc = 'Error propagation regression'
    license = 'gplv3+'
    inputs = ['logic']
    outputs = []
    channels = ({'id': 'data', 'name': 'Data', 'desc': 'Data'},)
    def reset(self):
        pass
    def start(self):
        pass
    def decode(self):
        self.wait()
        raise RuntimeError('Intentional regression test failure')
