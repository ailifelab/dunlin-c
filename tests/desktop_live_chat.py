#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import ctypes as C,json,pathlib,tempfile,secrets,os,sys
root=pathlib.Path(__file__).resolve().parents[1]
lib=C.CDLL(str(root/'build/desktop/libdesktop-test.dylib'));P=C.c_void_p;S=C.c_char_p
for name,args,ret in [('dc_open',[S],P),('dc_add_chat',[P,S,S,S],C.c_int),('dc_run',[P,S,S,S,P],P),('dc_messages',[P,S],P),('dc_free',[P],None),('dc_close',[P],None)]:
 f=getattr(lib,name);f.argtypes=args;f.restype=ret
EV=C.CFUNCTYPE(None,P,S,S);AP=C.CFUNCTYPE(C.c_int,P,S,S);CA=C.CFUNCTYPE(C.c_int,P)
class Hooks(C.Structure):_fields_=[('user',P),('event',EV),('approve',AP),('cancelled',CA)]
events=[]; proposals=[]
@EV
def event(ctx,name,text):events.append(name.decode())
@AP
def approval(ctx,name,args):
 proposals.append({'tool':name.decode(),'arguments':json.loads(args)})
 print('DENIED pending real-model tool:',name.decode(),flush=True);return 0
@CA
def cancel(ctx):return 0
hooks=Hooks(None,event,approval,cancel)
for line in (root/'dunlin-trial/model.env').read_text().splitlines():
 if line.startswith('OMLX_API_KEY='):os.environ['OMLX_API_KEY']=line.split('=',1)[1]
cfg=json.loads((root/'config/qwen-local-task.json').read_text());cfg.update(stream=True,max_tokens=512,task_timeout_ms=180000,timeout_ms=120000,max_context_bytes=65536,max_messages=128,max_calls=8)
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
with tempfile.TemporaryDirectory(prefix='dunlin-live-chat-') as temp:
 p=pathlib.Path(temp);p.chmod(0o700);w=p/'workspace';w.mkdir();h=lib.dc_open(temp.encode());assert h;assert lib.dc_add_chat(h,b'live',b'Live isolated chat',str(w).encode())
 word='DUNLIN-'+secrets.token_hex(4)
 def run(model,prompt):
  cfg['model']=model;events.clear();result=value(lib.dc_run(h,b'live',json.dumps(cfg).encode(),prompt.encode(),C.byref(hooks)));print(json.dumps({'model':model,'status':result['status'],'text':result['text'],'events':sorted(set(events))},ensure_ascii=False),flush=True);assert result['status']=='completed',result;assert 'tool.start' not in events;return result['text']
 first=run('Qwen3.5-4B-Uncensored-MLX-4bit','普通聊天测试，请记住校验词 '+word+'，只回复这个校验词，不要调用任何工具。');assert word in first
 second=run('gemma-4-E4B-it-MLX-4bit','上一条用户消息中的校验词是什么？仅原样回复，不要调用工具。');assert word in second
 third=run('Qwen3.5-4B-Uncensored-MLX-4bit','再核对一次最早那条消息里的校验词，只回复原文，不要工具。');assert word in third
 messages=value(lib.dc_messages(h,b'live'));assert len([x for x in messages if x['role']=='user'])==3
 print('PASS real Qwen/Gemma/Qwen streaming chat, unpredictable context retained, no tool dispatch',flush=True)
 assert lib.dc_add_chat(h,b'denial',b'Live permission denial',str(w).encode())
 cfg['model']='Qwen3.5-4B-Uncensored-MLX-4bit';proposals.clear()
 result=value(lib.dc_run(h,b'denial',json.dumps(cfg).encode(),'请调用 shell 工具运行 printf dunlin-authorization-test > denied-command.txt 这一个命令，不要做其他操作。'.encode(),C.byref(hooks)))
 assert proposals and any(p['tool'] in ['shell','exec'] for p in proposals),result
 assert not list(w.iterdir()),'denied command must not produce workspace effects'
 print(json.dumps({'permission_denial':result['status'],'proposals':proposals,'workspace_effects':[]},ensure_ascii=False),flush=True)
 lib.dc_close(h);print('PASS real-model proposed command denied before execution; no workspace effects',flush=True)
os.environ.pop('OMLX_API_KEY',None)
