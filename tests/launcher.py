"""Check public launcher option translation. The Docker boundary only records argv."""
import os
import subprocess
import tempfile
import shutil
from pathlib import Path

launch = str(Path(__file__).resolve().parents[1]/'scripts/qwen-chat.sh')
with tempfile.TemporaryDirectory() as directory:
    docker = Path(directory)/'docker'
    docker.write_text('#!/bin/sh\ncase "$1" in\ncontainer) exit 0;;\ninspect) echo true; exit 0;;\nexec) [ "$2" = -it ] || exit 0;;\nesac\nprintf "%s\\n" "$@"\n')
    docker.chmod(0o755)
    env = dict(os.environ, DUNLIN_TRIAL_DIR=directory, PATH=directory+os.pathsep+os.environ['PATH'])
    for args,strict,suite in [([],False,'bionic'), (['--auto'],False,'bionic'),
                              (['--linux','--task'],True,'bionic'), (['--task'],True,'bionic'),
                              (['--noble'],False,'noble')]:
        cmd=subprocess.check_output([launch,'test']+args,env=env).decode().splitlines()
        assert ('--require-tool' in cmd)==strict, (args,cmd)
        if strict: assert cmd[cmd.index('--require-tool')+1]=='shell'
        assert cmd[-1]=='chat' and '--events' not in cmd and '--json' not in cmd,cmd
        assert cmd[:3]==['exec','-it','dunlin-agent-dev'],cmd
        assert '/opt/dunlin/dunlin' in cmd,cmd
        assert 'create' not in cmd and 'run' not in cmd,cmd
        assert cmd[cmd.index('--permissions')+1]=='read-only'
        assert cmd[cmd.index('--approval-default')+1]=='no'
        assert cmd[cmd.index('--session')+1]=='test'
    for args in [[],['--linux'],['--noble'],['--auto'],['--task']]:
        cmd=subprocess.check_output([launch]+args,env=env).decode().splitlines()
        assert '--session' not in cmd,cmd
    cmd=subprocess.check_output([launch,'--session','explicit','--linux'],env=env).decode().splitlines()
    assert cmd[cmd.index('--session')+1]=='explicit'
    custom=subprocess.check_output([launch,'--permissions','strict','--approval-default','yes'],env=env).decode().splitlines()
    assert custom[custom.index('--permissions')+1]=='strict'
    assert custom[custom.index('--approval-default')+1]=='yes'
    for flag in ('--json','--events'):
        machine=subprocess.check_output([launch,flag,'--model','second'],env=env).decode().splitlines()
        assert flag in machine and machine[-1]=='chat',machine
        assert machine[machine.index('--model')+1]=='second',machine
    assert subprocess.run([launch,'--help'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE).returncode==0
    for args in [['--permissions','bogus'],['--approval-default','bogus'],['--session'],['--session','--mac'],['--session','one','two'],['--model'],['--model','--mac'],['--unknown']]:
        assert subprocess.run([launch]+args,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE).returncode==64
    # Exercise the Mac exec branch with an isolated fake install and literal env.
    # No deployed binary, private config, Docker container or model is started.
    isolated = Path(directory)/'install'
    (isolated/'scripts').mkdir(parents=True)
    shutil.copyfile(launch, isolated/'scripts/qwen-chat.sh')
    (isolated/'scripts/qwen-chat.sh').chmod(0o755)
    mac = isolated/'dist/0.2.12/macos-arm64/dunlin'
    mac.parent.mkdir(parents=True)
    mac.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
    mac.chmod(0o755)
    (Path(directory)/'model.env').write_text('OMLX_API_KEY=literal-$(not-executed)\n')
    uname = Path(directory)/'uname'
    uname.write_text('#!/bin/sh\necho Darwin\n')
    uname.chmod(0o755)
    for flags in ([], ['--json'], ['--events'], ['--events','--json']):
        cmd=subprocess.check_output([str(isolated/'scripts/qwen-chat.sh'),'--mac','--model','second']+flags,env=env).decode().splitlines()
        assert cmd[-1]=='chat' and cmd[cmd.index('--model')+1]=='second',cmd
        assert ('--events' in cmd)==('--events' in flags),cmd
        assert ('--json' in cmd)==('--json' in flags and '--events' not in flags),cmd
print('PASS launcher natural output default, explicit JSON/events and model, legacy modes and Linux targets')
