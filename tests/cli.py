#!/usr/bin/env python3
"""Isolated Linux CLI checks; no endpoint connection required."""
import json
import os
import subprocess
import sys
import tempfile

binary = os.path.abspath(sys.argv[1])
with tempfile.TemporaryDirectory() as directory:
    path = os.path.join(directory, 'profile.json')

    def invoke(*args, **opts):
        p = subprocess.run([binary, '--config', path, '--json'] + list(args),
                           cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert p.returncode == opts.get('code', 0), (args, p.returncode, p.stdout, p.stderr)
        return json.loads(p.stdout.decode())

    assert invoke('version')['version'] == '0.2.12'
    for help_arg in ['help','--help','-h']:
        assert '/permissions reset' in invoke(help_arg)['help']
    assert not os.path.exists(path)
    invoke('init')
    with open(path) as f:
        original = json.load(f)
    assert os.stat(path).st_mode & 0o777 == 0o600
    invoke('init', code=64)
    assert invoke('validate')['status'] == 'valid'
    for permission in ['request','agent','full-access','read-only','strict']:
        assert invoke('--permissions', permission, 'validate')['status'] == 'valid'
    assert invoke('doctor')['network_probed'] is False
    assert len(invoke('plugins')) == 12
    assert len(invoke('tools')) == 6
    assert invoke('sessions') == []
    def profile(**overrides):
        document = original.copy()
        document.update(overrides)
        with open(path, 'w') as f:
            json.dump(document, f)
    profile(enabled_tools=['echo'])
    assert [x['function']['name'] for x in invoke('tools')] == ['echo']
    assert 'tool.exec' not in invoke('plugins')
    profile(plugins=['clock'])
    assert invoke('plugins') == ['clock']
    invoke('run', 'hello', code=1)
    for bad in [dict(plugins=['loop']), dict(plugins=['clock','clock']),
                dict(enabled_tools=['bogus']), dict(timeout_ms=True), dict(max_rounds=0), dict(max_tokens=True), dict(max_tokens=0),
                dict(omlx_chat_template=42), dict(omlx_chat_template='x'*8193), dict(omlx_chat_template=''), dict(enable_thinking='false'), dict(temperature=True), dict(temperature=-1), dict(tool_result_for=42), dict(tool_result_for='shell'), dict(temperature=2.1),
                dict(workspace='.'), dict(workspace='work',session_dir='work/state'),
                dict(key_env='a-b'), dict(base_url='http://user:pass@localhost/x'),
                dict(base_url='file:///tmp'), dict(steam=True), dict(key='secret')]:
        profile(**bad)
        invoke('validate', code=64)
    for invalid in ['{"model": 1, "model": 2}', '{}#', '{"plugins": null}']:
        with open(path,'w') as f:
            f.write(invalid)
        invoke('validate', code=64)
    profile()
    p = subprocess.run([binary,'--config',path,'--json','resume'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert p.returncode==64 and p.stderr
    print('CLI PASS: init/no overwrite, modes, JSON output, limits, isolated paths, services, tool selection, invalid options and missing dependencies')
    os.unlink(path)
    os.mkfifo(path)
    invoke('validate',code=64)
