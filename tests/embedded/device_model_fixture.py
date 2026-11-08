"""Explicit on-device negative-model fixture; logs only request count, no headers."""
import argparse,json,time
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
p=argparse.ArgumentParser();p.add_argument('--bind',required=True);p.add_argument('--allow',required=True);p.add_argument('--port',type=int,default=1236);a=p.parse_args()
class Handler(BaseHTTPRequestHandler):
    count=0
    def log_message(self,*args):pass
    def do_POST(self):
        if self.client_address[0]!=a.allow:self.send_error(403);return
        length=int(self.headers.get('Content-Length','0'))
        if length<0 or length>8192:self.send_error(413);return
        request=json.loads(self.rfile.read(length));Handler.count+=1
        model=request.get('model')
        if model=='stall':time.sleep(35);body=b'{}'
        elif model=='oversize':body=b' '*9000
        elif model=='deep':body=b'['*20+b']'*20
        else:
            body=json.dumps({'choices':[{'message':{'role':'assistant','content':'','tool_calls':[{'id':'unsupported-'+str(Handler.count),'type':'function','function':{'name':'shell','arguments':json.dumps({'command':'uname -a; apt install nodejs'})}}]}}]}).encode()
        self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers()
        try:self.wfile.write(body)
        except (BrokenPipeError,ConnectionResetError):pass
        print('fixture_request='+str(Handler.count)+' mode='+str(model),flush=True)
ThreadingHTTPServer((a.bind,a.port),Handler).serve_forever()
