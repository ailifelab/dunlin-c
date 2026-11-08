#!/usr/bin/env python3
"""Direct data-only C parser contracts; no interpreter, file or network resolver."""
import ctypes as C,json,pathlib,sys,concurrent.futures
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p
class Error(C.Structure):_fields_=[('code',C.c_int),('line',C.c_size_t),('column',C.c_size_t)]
lib.dc_static_config_parse.argtypes=[C.c_char_p,C.c_size_t,C.POINTER(Error)];lib.dc_static_config_parse.restype=P
lib.json_object_to_json_string_ext.argtypes=[P,C.c_int];lib.json_object_to_json_string_ext.restype=C.c_char_p
lib.json_object_put.argtypes=[P];lib.json_object_put.restype=C.c_int
def parse(source):
 raw=source.encode() if isinstance(source,str) else source;error=Error();value=lib.dc_static_config_parse(raw,len(raw),C.byref(error))
 if not value:return None,(error.code,error.line,error.column)
 try:return json.loads(lib.json_object_to_json_string_ext(value,0)),(error.code,error.line,error.column)
 finally:lib.json_object_put(value)
source='''# Static local configuration
providers:
  local:
    baseURL: http://127.0.0.1:8080/v1
    api: openai-completions
    apiKeyEnv: ${OWN_FIXTURE_ENV}
    models:
      - id: fixture
        contextWindow: 32768
        maxTokens: 4096
        reasoning: false
        input: [text, image]
        name: "本地模型"
        prompt: |
          Preserve original meaning.
          Keep negative and conditional clauses.
metadata: {quoted: "true", plain: true, blank: null, leading: 01}
'''
value,error=parse(source);assert error[0]==0,error;model=value['providers']['local']['models'][0];assert model['name']=='本地模型' and model['reasoning'] is False and model['input']==['text','image'] and model['maxTokens']==4096 and model['prompt'].endswith('clauses.\n');assert value['providers']['local']['apiKeyEnv']=='${OWN_FIXTURE_ENV}' and value['metadata']==dict(quoted='true',plain=True,blank=None,leading='01')
assert parse(json.dumps(value,ensure_ascii=False))[0]==value
assert parse('%YAML 1.2\n---\na: 1\n')[0]=={'a':1}
print('PASS native C UTF-8 JSON/static YAML, nested providers/models, exact quoted and block text, JSON-compatible scalars; references are literal data')
cases=[('a: 1\na: 2\n',5),('{"nested":{"x":null,"x":3}}',5),('a: &anchor [1,2]\nb: *anchor\n',4),('a: *missing\n',4),('a: !!js "do not execute"\n',3),('a: !custom value\n',3),('<<: {a: 1}\n',4),('? [a,b]\n: complex key\n',6),('---\na: 1\n---\nb: 2\n',7),('%YAML 1.1\n---\na: on\n',7),('a: "\\0"\n',9),('a: 9223372036854775808\n',2),('a: 1e9999\n',2),('a: 1e-9999\n',2),('not a document',8),(b'a:\xff\n',9),(b'a: x\0y',9)]
for source,code in cases:
 value,error=parse(source);assert value is None and error[0]==code and error[1]>0 and error[2]>0,(repr(source),value,error)
assert parse('outer:\n  x: 1\n  x: 2\n')[1]==(5,3,3)
assert parse('a: ['*33+'0'+']'*33)[1][0]!=0
assert parse('['*34+'0'+']'*34)[1][0]==2
assert parse('['+','.join(['null']*65537)+']')[1][0]==2
assert parse('x'*1048577)[1][0]==2
print('PASS precise duplicate location, aliases/anchors/tags/merge/complex keys/multi-document refusal, UTF-8/NUL and integer/float overflow, source/depth/node limits')
def independent(i):
 source='root:\n  value: '+str(i)+'\n  text: "same"\n';value,error=parse(source);assert value==dict(root=dict(value=i,text='same')) and error[0]==0
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(independent,range(100)))
print('PASS independent parallel C parser instances without global parser mutation or side-effect resolution')
