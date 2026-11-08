import importlib.util,json,copy,tempfile
from pathlib import Path
spec=importlib.util.spec_from_file_location('device_config','scripts/generate-device-config.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
base=json.loads(Path('dunlin-copilot/esp32/esp32/config/python.json').read_text());assert module.validate(base) is base
for key,value in [('network_policy','disabled_tls'),('network_policy',True),('network_policy',None),('script_ms',999999),('script_ms',True),('stdout_bytes',-1),('code_bytes',9999),('max_allowed_hosts',0),('default_allowed_hosts',['https://example.com']),('default_allowed_hosts',['example.com','example.com']),('default_allowed_hosts',['evil.com\n#define X 1']),('micropython_version','unknown'),('ntp_server','pool.ntp.org/path')]:
 cfg=copy.deepcopy(base);cfg[key]=value
 try:module.validate(cfg)
 except ValueError:pass
 else:raise AssertionError((key,value))
assert 'AP_RESTRICT_HOSTS 0u' in module.render(base)
restricted=copy.deepcopy(base);restricted['network_policy']='allowlist';assert 'AP_RESTRICT_HOSTS 1u' in module.render(restricted)
assert module.render(base)==Path('dunlin-copilot/esp32/esp32/config/python_defaults.h').read_text()
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'generated.h';module.write_changed(p,'same');before=p.stat().st_mtime_ns;module.write_changed(p,'same');assert p.stat().st_mtime_ns==before
print('PASS generated-device-config bounds, type/version/host injection negatives, current header identity and unchanged-output timestamp preservation')
# Exercise the actual OpenAI generation entry point, including prompt-only isolation.
original_root=module.ROOT
with tempfile.TemporaryDirectory() as tmp:
    module.ROOT=Path(tmp)
    for rel in ['dunlin-copilot/esp32/esp32/config','dunlin-copilot/esp32/agent','dunlin-copilot/esp32/python']:(module.ROOT/rel).mkdir(parents=True,exist_ok=True)
    for rel in ['dunlin-copilot/esp32/esp32/config/python.json','dunlin-copilot/esp32/agent/system.txt','dunlin-copilot/esp32/python/system.txt']:(module.ROOT/rel).write_text((original_root/rel).read_text())
    api=module.ROOT/'dunlin-copilot/esp32/esp32/config/openai.json'
    good={'max_output_tokens':4096,'response_bytes':65536};api.write_text(json.dumps(good));module.main()
    assert (module.ROOT/'dunlin-copilot/esp32/esp32/config/openai_defaults.h').read_text()==(original_root/'dunlin-copilot/esp32/esp32/config/openai_defaults.h').read_text()
    for key,value in [('max_output_tokens',True),('max_output_tokens',0),('max_output_tokens',8193),('response_bytes',8191),('response_bytes',65537)]:
        bad=dict(good);bad[key]=value;api.write_text(json.dumps(bad))
        try:module.main()
        except ValueError:pass
        else:raise AssertionError((key,value))
    import sys
    saved_args=sys.argv[:]
    try:
        api.unlink();sys.argv=['generate-device-config.py','--prompt-only'];module.main()
    finally:sys.argv=saved_args
module.ROOT=original_root
print('PASS OpenAI generated bounds/type negatives, exact generated identity and ordinary-target independence')
