"""Exercise public-client OAuth revocation against an isolated TLS issuer."""
import ctypes as C
import http.server
import json
import os
import secrets
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from pathlib import Path

lib=C.CDLL(sys.argv[1]);P,S=C.c_void_p,C.c_char_p
for name,args,ret in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),
    ('dc_bind_identity',[P,S],C.c_int),('dc_add_chat',[P,S,S,S],C.c_int),
    ('dc_remote_account_record',[P,S,S,S,S,C.c_int64],C.c_int),
    ('dc_remote_account_revoke',[P,S,S,S],P),('dc_workspace_request',[P,S],P)]:
    f=getattr(lib,name);f.argtypes=args;f.restype=ret

def take(ptr):
    assert ptr
    try:return json.loads(C.string_at(ptr))
    finally:lib.dc_free(ptr)

tokens={};received=[]
class Issuer(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_POST(self):
        form=urllib.parse.parse_qs(self.rfile.read(int(self.headers['Content-Length'])).decode())
        token=form['token'][0];received.append(self.path)
        if self.path=='/calendar' and received.count('/calendar')==1:status=503
        elif tokens.get(token)==self.server.server_port:tokens.pop(token);status=200
        else:status=401
        self.send_response(status);self.send_header('Content-Length','0');self.end_headers()
    def do_GET(self):
        status=200 if self.headers.get('Authorization','')[7:] in tokens and tokens[self.headers['Authorization'][7:]]==self.server.server_port else 401
        self.send_response(status);self.send_header('Content-Length','0');self.end_headers()

with tempfile.TemporaryDirectory() as tmp:
    root=Path(tmp);crt=root/'cert';key=root/'key'
    config=root/'openssl.cnf'
    config.write_text('[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=localhost\n[ext]\nsubjectAltName=IP:127.0.0.1\nbasicConstraints=critical,CA:TRUE\n')
    subprocess.run(['openssl','req','-new','-x509','-newkey','rsa:2048','-nodes','-days','1','-config',config,'-keyout',key,'-out',crt],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    os.environ['SSL_CERT_FILE']=str(crt)
    services={}
    endpoints={}
    for account in ('calendar','files'):
        server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Issuer)
        ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(crt,key)
        server.socket=ctx.wrap_socket(server.socket,server_side=True)
        threading.Thread(target=server.serve_forever,daemon=True).start()
        services[account]=server
        endpoints[account]=f'https://127.0.0.1:{server.server_port}/{account}'
    def access(account,token):
        import http.client
        client=http.client.HTTPSConnection('127.0.0.1',services[account].server_port,context=ssl.create_default_context(cafile=str(crt)))
        client.request('GET','/protected',headers={'Authorization':'Bearer '+token})
        response=client.getresponse();response.read();client.close();return response.status
    state=root/'state';state.mkdir(mode=0o700);db=lib.dc_open(str(state).encode())
    assert db and lib.dc_bind_identity(db,b'alice') and lib.dc_add_chat(db,b'chat',b'Grant review',b'')
    expiry=int(time.time())+500
    issued={}
    for account in ('calendar','files'):
        token=secrets.token_urlsafe(32);tokens[token]=services[account].server_port;issued[account]=token
        assert access(account,token)==200
        assert lib.dc_remote_account_record(db,account.encode(),b'chat',account.encode(),endpoints[account].encode(),expiry)
    assert not lib.dc_remote_account_record(db,b'bad',b'chat',b'bad',b'http://example.invalid',expiry)
    assert not lib.dc_remote_account_revoke(db,b'calendar',endpoints['files'].encode(),issued['calendar'].encode())
    assert not received
    def revoke(account):
        rows=take(lib.dc_remote_account_revoke(db,account.encode(),endpoints[account].encode(),issued[account].encode()))
        return next(r['state'] for r in rows if r['id']==account)
    assert [revoke(a) for a in issued]==['retry','revoked']
    assert access('calendar',issued['calendar'])==200 and access('files',issued['files'])==401
    lib.dc_close(db);db=lib.dc_open(str(state).encode())
    assert revoke('calendar')=='revoked' and access('calendar',issued['calendar'])==401
    assert received==['/calendar','/files','/calendar']
    assert revoke('calendar')=='revoked' and received.count('/calendar')==2
    lib.dc_close(db)
    for server in services.values():server.shutdown();server.server_close()
    for path in state.glob('*.sqlite*'):
        assert all(token.encode() not in path.read_bytes() for token in issued.values())
print('PASS TLS issuer accepted both tokens before removal, one failed and retried after restart, both protected resources rejected tokens, completed item not replayed')
