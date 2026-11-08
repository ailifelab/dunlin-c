"""argv: C measure driver, CLI. Linux, an isolated writable temp directory."""
import json
import os
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from mock_model import MockServer

measure, binary = map(os.path.abspath, sys.argv[1:])
server = MockServer()
threading.Thread(target=server.serve_forever, daemon=True).start()
base = 'http://127.0.0.1:{}/v1'.format(server.server_port)
n = 25
def stats(values):
    return [round(statistics.median(values),3), round(sorted(values)[int(.95*len(values)+.999)-1],3)]
with tempfile.TemporaryDirectory() as d:
    os.mkdir(os.path.join(d,'work'))
    cfg=os.path.join(d,'dunlin.json')
    with open(cfg,'w') as f:
        json.dump(dict(base_url=base,model='mock-local',stream=False),f)
    for name,cmd in [('version',['version']),('assemble',['doctor']),('one-request',['run','hello']),
                     ('two-requests-two-tools',['run','multiple'])]:
        stdout=subprocess.check_output([measure,str(n),binary,'--config',cfg,'--json']+cmd)
        rows=[list(map(float,row.split())) for row in stdout.decode().splitlines()]
        print('{} wall_ms_p50_p95={} peak_rss_KiB_p50_p95={}'.format(name,stats([x[0] for x in rows]),stats([x[1] for x in rows])))
    # Independent wire fixture baseline; no Agent, journal or CLI process launch.
    raw=json.dumps(dict(model='mock-local',stream=False,messages=[dict(role='user',content='hello')])).encode()
    url=base+'/chat/completions'
    samples=[]
    for i in range(n+1):
        start=time.monotonic()
        with urllib.request.urlopen(urllib.request.Request(url,data=raw,headers={'Content-Type':'application/json'})) as r:
            assert json.load(r)['choices'][0]['message']['content']=='reply:hello'
        if i: samples.append((time.monotonic()-start)*1000)
    print('raw_python_http_ms_p50_p95={}'.format(stats(samples)))
server.shutdown()
