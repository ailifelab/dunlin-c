#!/usr/bin/env python3
"""Signed offline fixtures, actual Ed25519 verifier, C archive validation and Skills import."""
import ctypes as C,json,sys,tempfile,pathlib,subprocess,base64,hashlib,time,copy,sqlite3
lib=C.CDLL(sys.argv[1]);P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_workspace_request',[P,S],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
sha=lambda b:hashlib.sha256(b).hexdigest()
encode=lambda p:json.dumps(p,ensure_ascii=False,separators=(',',':')).encode()
with tempfile.TemporaryDirectory(prefix='dunlin-catalog-') as tmp:
 root=pathlib.Path(tmp);private=root/'private';private.mkdir(mode=0o700);key=root/'fixture-key.pem';pub=root/'pub.der';openssl='/opt/homebrew/opt/openssl@3/bin/openssl'
 subprocess.run([openssl,'genpkey','-algorithm','ed25519','-out',str(key)],check=True,capture_output=True);subprocess.run([openssl,'pkey','-in',str(key),'-pubout','-outform','DER','-out',str(pub)],check=True,capture_output=True);public=pub.read_bytes()[-32:].hex()
 db=lib.dc_open(str(private).encode());assert db
 def req(op,**kw):
  raw=lib.dc_workspace_request(db,encode(dict(op=op,**kw)));assert raw
  try:return json.loads(C.string_at(raw))
  finally:lib.dc_free(raw)
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r.get('data')
 def bad(op,error=None,**kw):
  r=req(op,**kw);assert not r['ok'],r
  if error:assert r['error']==error,r
  return r
 ok('catalog.add',id='test',title='Owned fixture',url='https://catalog.invalid/v1.json',publicKey=public)
 bad('catalog.add',id='insecure',title='bad',url='http://catalog.invalid',publicKey=public)
 def signed(payload):
  b=payload if isinstance(payload,bytes) else encode(payload);(root/'payload').write_bytes(b);subprocess.run([openssl,'pkeyutl','-sign','-rawin','-inkey',str(key),'-in',str(root/'payload'),'-out',str(root/'signature')],check=True,capture_output=True)
  return encode({'algorithm':'Ed25519','payload':base64.b64encode(b).decode(),'signature':(root/'signature').read_bytes().hex()}).decode()
 manifest={'packageId':'review-c','version':'1.0.0','kind':'skill','engines':{'dunlin':'desktop@1'},'platforms':['macos','linux','windows'],'architectures':['arm64','x86_64'],'dependencies':[],'capabilities':[]}
 skill=b'---\nname: C review\nversion: 1.0.0\ndescription: fixture skill\n---\nPreserve original file semantics.\n'
 archive={'protocol':'dunlin.files','major':1,'manifest':manifest,'manifestJSON':encode(manifest).decode(),'files':[{'path':'SKILL.md','data':base64.b64encode(skill).decode(),'digest':sha(skill)},{'path':'assets/empty','data':'','digest':sha(b'')}]};raw_archive=encode(archive)
 version={'version':'1.0.0','digest':sha(raw_archive),'manifestDigest':sha(encode(manifest)),'artifactSource':{'type':'https_immutable_archive','format':'dunlin.files.v1','url':'https://catalog.invalid/review-c.json'},**{k:manifest[k] for k in ['engines','platforms','architectures','dependencies','capabilities']}}
 payload={'protocol':'dunlin.catalog','major':1,'catalogId':'owned-test','title':'Owned test','publisher':'Fixture','keyId':'fixture-1','revision':1,'issuedAt':int(time.time())-1,'expiresAt':int(time.time())+3600,'entries':[{'packageId':'review-c','kind':'skill','displayName':'Review C','publisher':'Fixture','license':'MIT','versions':[version]}],'withdrawals':[],'nextPage':None}
 envelope=signed(payload);ok('catalog.verify',id='test',envelope=envelope);ok('catalog.verify',id='test',envelope=envelope)
 tampered=json.loads(envelope);tampered['signature']='00'*64;bad('catalog.verify','invalid_signature_or_envelope',id='test',envelope=encode(tampered).decode())
 changed=copy.deepcopy(payload);changed['title']='changed';bad('catalog.verify','catalog_revision_rollback_or_conflict',id='test',envelope=signed(changed))
 older=copy.deepcopy(payload);older['revision']=0;bad('catalog.verify','catalog_revision_rollback_or_conflict',id='test',envelope=signed(older))
 expired=copy.deepcopy(payload);expired['revision']=2;expired['issuedAt']=int(time.time())-200;expired['expiresAt']=int(time.time())-1;bad('catalog.verify','invalid_or_expired_catalog',id='test',envelope=signed(expired))
 duplicate=encode(payload).replace(b'"major":1',b'"major":1,"major":1');bad('catalog.verify','invalid_or_expired_catalog',id='test',envelope=signed(duplicate))
 bad('catalog.install','package_digest_mismatch',source='test',package='review-c',version='1.0.0',archive='{}')
 ok('catalog.install',source='test',package='review-c',version='1.0.0',archive=raw_archive.decode());rows=ok('skills.list',scope='global',scopeID='');assert len(rows)==1 and rows[0]['source']=='catalog:test' and not rows[0]['enabled'];assert rows[0]['selected']['resources'][0]['path']=='SKILL.md'
 ok('skills.bind',scope='global',scopeID='',source='catalog:test',package='review-c',mode='enable',version='1.0.0');assert ok('skills.list',scope='global',scopeID='')[0]['enabled']
 print('PASS pinned Ed25519, exact signed bytes, duplicate/expiry/revision rejection, archive SHA and immutable disabled-by-default Skill installation')
 # Re-signing malformed archives must not bypass path or actual-manifest checks.
 malicious=copy.deepcopy(archive);malicious['files'][0]['path']='../escape';raw=encode(malicious);p=copy.deepcopy(payload);p['revision']=2;p['entries'][0]['versions'][0]['digest']=sha(raw);bad('catalog.verify','immutable_catalog_version_changed',id='test',envelope=signed(p));ok('catalog.add',id='unsafe',title='Unsafe fixture',url='https://catalog.invalid/unsafe.json',publicKey=public);ok('catalog.verify',id='unsafe',envelope=signed(p));bad('catalog.install','unsafe_or_incomplete_archive',source='unsafe',package='review-c',version='1.0.0',archive=raw.decode());assert not (private/'escape').exists()
 ok('catalog.remove',id='test');assert ok('skills.list',scope='global',scopeID='')[0]['enabled'];bad('catalog.verify','source_missing_or_disabled',id='test',envelope=envelope);ok('catalog.restore',id='test')
 p=copy.deepcopy(payload);p['revision']=3;p['withdrawals']=[{'packageId':'review-c','version':'1.0.0','reason':'fixture withdrawal'}];ok('catalog.verify',id='test',envelope=signed(p));bad('catalog.install','version_withdrawn',source='test',package='review-c',version='1.0.0',archive=raw_archive.decode())
 assert not ok('skills.list',scope='global',scopeID='')[0]['enabled'] and ok('skills.list',scope='global',scopeID='')[0]['blocked']
 p=copy.deepcopy(payload);p['revision']=4;p['entries'][0]['packageId']='fixture-plugin';p['entries'][0]['kind']='plugin';ok('catalog.verify',id='test',envelope=signed(p));bad('catalog.install','extension_host_unsupported',source='test',package='fixture-plugin',version='1.0.0',archive=raw_archive.decode())
 lib.dc_close(db);db=lib.dc_open(str(private).encode());assert db;assert ok('catalog.list')[0]['revision']==4;lib.dc_close(db)
 print('PASS signed traversal rejection, withdrawal/install gating, retained installed content after source removal, plugin gate and restart')
