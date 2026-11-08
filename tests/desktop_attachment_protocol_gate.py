#!/usr/bin/env python3
"""Do not confuse declared vendor image ability with an installed wire codec."""
import ctypes as C,json,pathlib,struct,sys,tempfile,zlib
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for name,args,result in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_add_chat',[P,S,S,S],C.c_int),('dc_workspace_request',[P,S],P),('dc_attachment_stage',[P,S,S,S,S,C.c_int,P,C.c_size_t],P),('dc_run_message_with_secret',[P,S,S,S,P,S,S,C.POINTER(C.c_int)],P)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=result
def val(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
def enc(v):return json.dumps(v,ensure_ascii=False).encode()
def chunk(k,d):return struct.pack('>I',len(d))+k+d+struct.pack('>I',zlib.crc32(k+d))
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1,1,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0\x17\x2a\x63\xff'))+chunk(b'IEND',b'')
with tempfile.TemporaryDirectory(prefix='dunlin-image-codec-gate-') as tmp:
 root=pathlib.Path(tmp);data=root/'private';data.mkdir(mode=0o700);work=root/'project';work.mkdir();db=lib.dc_open(str(data).encode());assert db
 def req(op,**kw):return val(lib.dc_workspace_request(db,enc(dict(op=op,**kw))))
 models=[]
 for protocol in ['chat','responses','anthropic','gemini','ollama']:
  models.append(dict(id=protocol,name=protocol,endpoint='http://127.0.0.1:1/v1',model='fixture',protocolName=protocol,maxOutput=256,inputModalities=['text','image']))
 registry=req('config.save',revision=0,models=models,providers=[]);assert registry['ok'];revision=registry['data']['revision']
 for protocol in ['responses','anthropic','gemini','ollama']:
  chat=protocol.encode();assert lib.dc_add_chat(db,chat,chat,str(work).encode());a=val(lib.dc_attachment_stage(db,chat,None,b'picture.png',b'file',1,png,len(png)));assert a['ok']
  cfg=dict(base_url='http://127.0.0.1:1/v1',model='fixture',protocol=protocol,stream=False,max_tokens=256,desktop_tools='noTools',desktop_input_modalities=['text','image'])
  committed=C.c_int(99);result=val(lib.dc_run_message_with_secret(db,chat,enc(cfg),b'reviewed image',None,b'',a['data']['id'].encode(),C.byref(committed)))
  assert result['status']=='failed' and committed.value==0,('unsupported image codec was admitted and consumed pending input',protocol,result,committed.value)
  assert req('attachments.pending',chat=protocol)['data']['id']==a['data']['id']
  plan=req('chats.model.preview',chat=protocol,target=protocol,revision=revision);assert plan['ok'] and not plan['data']['canSwitch'] and plan['data']['reason']=='model_switch_image_protocol_unsupported',plan
  plan=req('chats.model.preview',chat=protocol,target=protocol,revision=revision,imagePolicy='textOnly');assert plan['ok'] and plan['data']['canSwitch'],plan
 lib.dc_close(db)
print('PASS unsupported image serializers reject before canonical input consumption; source/draft preserved and explicit text-only review remains available')
