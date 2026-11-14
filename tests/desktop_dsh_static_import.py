#!/usr/bin/env python3
"""Fixed DSH explicit-provider subset, normalized by C without execution/resolution."""
import ctypes as C,json,pathlib,tempfile,sys,sqlite3
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()));P=C.c_void_p;S=C.c_char_p
for n,a,r in [('dc_open',[S],P),('dc_close',[P],None),('dc_free',[P],None),('dc_workspace_request',[P,S],P)]:f=getattr(lib,n);f.argtypes=a;f.restype=r
def value(p):
 assert p
 try:return json.loads(C.string_at(p))
 finally:lib.dc_free(p)
with tempfile.TemporaryDirectory(prefix='dunlin-dsh-static-') as tmp:
 state=pathlib.Path(tmp)/'private';state.mkdir(mode=0o700);db=lib.dc_open(str(state).encode());assert db
 def req(op,**kw):return value(lib.dc_workspace_request(db,json.dumps(dict(op=op,**kw),ensure_ascii=False).encode()))
 def ok(op,**kw):
  r=req(op,**kw);assert r['ok'],r;return r.get('data')
 original=ok('config.save',revision=0,models=[],providers=[])
 source='''providers:
  local:
    displayName: 本机服务
    baseURL: http://127.0.0.1:8080/v1
    api: openai-completions
    apiKeyEnv: OWN_FIXTURE_MODEL_KEY
    apiKey: FIXTURE_VALUE_MUST_NOT_BE_SAVED
    models:
      - id: qwen-fixture
        name: 本地模型
        contextWindow: 32768
        maxTokens: 8192
        reasoning: true
        input: [text, image]
        cost: {input: 0, output: 0}
defaultModel: local/qwen-fixture
'''
 plan=ok('config.preview',source=source);assert ok('config.list')==original
 p=plan['providers'][0];m=plan['models'][0];assert p['routeID']=='local' and p['name']=='本机服务' and p['protocolName']=='chat' and p['keyEnvironment']=='OWN_FIXTURE_MODEL_KEY' and 'credentialID' not in p
 assert plan['sourceProviders']=={p['id']:{'path':'$.providers.local','api':'openai-completions'}}
 assert m['providerID']==p['id'] and m['contextLimit']==32768 and m['advertisedOutputLimit']==8192 and m['maxOutput']==4096 and m['reasoningSupported'] is True and m['inputModalities']==['text','image'] and '尚未联调' in m['contextSource']
 assert set(['default_model_not_replaced','credential_value_omitted','pricing_not_imported'])<=set(plan['warnings']);assert 'FIXTURE_VALUE_MUST_NOT_BE_SAVED' not in json.dumps(plan)
 saved=ok('config.apply',plan=plan['plan'],confirmed=True);assert len(saved['models'])==1
 again=ok('config.preview',source=source);assert again['providers'][0]['routeID'].startswith('local-imported-') and again['providers'][0]['id']!=p['id'];assert again['sourceProviders'][again['providers'][0]['id']]['path']=='$.providers.local';assert 'provider_alias_renamed' in again['warnings'];saved=ok('config.apply',plan=again['plan'],confirmed=True)
 print('PASS data-only YAML DSH provider/model field mapping, capability declaration labels, explicit no-default-replacement warnings and fresh aliases/identities')
 provider=dict(baseURL='https://example.com/v1',api='openai-responses',models=[dict(id='model',contextWindow=8192,maxTokens=2048,input=['text'])])
 wrapped=[dict(id='ui-unrelated',disabled=False,config=dict(arbitrary='ignored without execution')),dict(id='llm-pi-ai',config=dict(providers=dict(remote=provider)))]
 plan=ok('config.preview',source=json.dumps(wrapped));assert plan['providers'][0]['protocolName']=='responses' and 'plugin_declaration_not_installed' in plan['warnings'];assert plan['models'][0]['maxOutput']==2048;assert plan['sourceProviders'][plan['providers'][0]['id']]=={'path':'$[1].config.providers.remote','api':'openai-responses'}
 bad_sources=[
  (dict(providers=dict(x=dict(provider,modelOverrides={}))), 'explicit_models_required'),
  (dict(providers=dict(x=dict(api='openai-completions',baseURL='https://example.com/v1'))), 'explicit_models_required'),
  (dict(providers=dict(x=dict(provider,api='unsupported-api'))), 'explicit_protocol_required'),
  (dict(providers=dict(x={k:v for k,v in provider.items() if k!='baseURL'})), 'explicit_endpoint_required'),
  (dict(providers=dict(x=dict(provider,headers=dict(Authorization='fixture-not-imported')))), 'unsupported_field'),
  ([dict(id='llm-pi-ai',name='arbitrary-code.js',config=dict(providers=dict(x=provider)))], 'unsupported_field'),
  ([dict(id='other',config=dict(include='must-not-read.yml')),dict(id='llm-pi-ai',config=dict(providers=dict(x=provider)))], 'dynamic_include_not_supported'),
  (dict(providers=dict(x=dict(provider,models=[dict(id='same'),dict(id='same')]))), 'duplicate_model_id'),
  (dict(providers=dict(x=dict(provider,models=[dict(id='one',input=['video'])]))), 'unsupported_modality'),
  (dict(providers=dict(x=dict(provider,models=[dict(id='one',input=['image'])]))), 'text_modality_required'),
  (dict(providers=dict(x=dict(provider,models=[dict(id='one',api='openai-completions')]))), 'model_protocol_override_not_supported'),
  (wrapped+[dict(id='llm-pi-ai',config=dict(providers=dict(second=provider)))], 'multiple_provider_blocks_not_supported'),
 ]
 for document,code in bad_sources:
  result=req('config.preview',source=json.dumps(document));assert not result['ok'] and result['diagnostic']['code']==code,(document,result);assert result['diagnostic']['path']
 for source,code in [('providers: !!js dangerous','static_tag'),('a: 1\na: 2','static_duplicate'),('x: &a [1,2]\ny: *a','static_alias')]:
  result=req('config.preview',source=source);assert not result['ok'] and result['diagnostic']['code']==code and result['diagnostic']['line']>0,result
 assert ok('config.list')==saved
 small=dict(providers=dict(small=dict(provider,models=[dict(id='small',contextWindow=1024)])))
 plan=ok('config.preview',source=json.dumps(small));assert plan['models'][0]['maxOutput']==256,plan
 smallest=dict(providers=dict(small=dict(provider,models=[dict(id='small',contextWindow=514)])))
 plan=ok('config.preview',source=json.dumps(smallest));assert plan['models'][0]['maxOutput']==1,plan
 too_small=dict(providers=dict(small=dict(provider,models=[dict(id='small',contextWindow=513)])))
 result=req('config.preview',source=json.dumps(too_small));assert not result['ok'] and result['diagnostic']['code']=='invalid_context_window',result
 nested=dict(include='must-not-read')
 for i in range(9):nested={'界'*24:nested}
 result=req('config.preview',source=json.dumps([dict(id='unrelated',config=nested),wrapped[1]],ensure_ascii=False));assert not result['ok'] and result['diagnostic']['code']=='dynamic_include_not_supported' and 'include' in result['diagnostic']['path'],result
 print('PASS small declared windows receive feasible bounded output recommendations and deeply nested Chinese diagnostics remain valid UTF-8')
 with sqlite3.connect(state/'conversations.sqlite') as sql:assert 'FIXTURE_VALUE_MUST_NOT_BE_SAVED' not in '\n'.join(sql.iterdump())
 print('PASS unsupported/dynamic fields, catalog-dependent overrides, unknown protocols, duplicates/modalities and YAML errors report stable paths/locations without changing registry or saving credential values')
 exported=dict(format='dunlin.config',version=1,models=saved['models'],providers=saved['providers']);roundtrip=ok('config.preview',source=json.dumps(exported));assert len({p['routeID'] for p in roundtrip['providers']})==2 and not set(p['routeID'] for p in saved['providers'])&set(p['routeID'] for p in roundtrip['providers']);assert all('credentialID' not in p for p in roundtrip['providers'])
 saved=ok('config.apply',plan=roundtrip['plan'],confirmed=True);lib.dc_close(db);db=lib.dc_open(str(state).encode());assert db;assert ok('config.list')==saved;lib.dc_close(db)
 print('PASS portable static reimport preserves routes as new identities and remains stable across reopen')
