#!/usr/bin/env python3
"""Run deterministic Agent acceptance in fresh directories; never launch a model.

Development tool (Python 3.6+), not part of the C runtime. Each command, exit,
log, input and artifact hash is retained. A failed group does not hide other
independent groups. This does not edit the requirement ledger automatically.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
GROUPS = ('test', 'test-agent-contracts', 'test-runtime-profiles',
          'test-protocol', 'test-shell-profiles', 'test-headers', 'test-embedded')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs():
    paths = [ROOT / 'Makefile']
    for folder in ('include', 'dunlin-compact', 'dunlin-command', 'dunlin-copilot/esp32', 'tests', 'scripts'):
        for directory, subdirs, files in os.walk(str(ROOT / folder)):
            subdirs[:] = [s for s in subdirs if s not in ('.pio', '__pycache__')]
            paths.extend(Path(directory) / name for name in files
                         if Path(name).suffix in ('.c', '.h', '.py', '.sh', '.trace', '.ini', '.mjs'))
    return {str(p.relative_to(ROOT)): digest(p) for p in sorted(set(paths))}


def stamp():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def toolchain():
    result = []
    for command in (['cc', '--version'], ['pkg-config', '--modversion', 'json-c', 'libcurl']):
        try:
            probe = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
            result.append(dict(command=command, exit_code=probe.returncode,
                               output=probe.stdout.decode('utf-8', errors='replace').strip()))
        except (OSError, subprocess.TimeoutExpired) as error:
            result.append(dict(command=command, error=str(error)))
    return result


def terminate_group(process):
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass
    # The make/group leader can exit before a child that ignores TERM.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def interrupted(signum, frame):
    raise KeyboardInterrupt('signal {}'.format(signum))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, help='new, dedicated build directory')
    parser.add_argument('--report', required=True, help='new, dedicated evidence directory')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--only', action='append', choices=GROUPS, help='run a stated subset')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Python assertions must remain enabled')
    build, report = Path(args.build).resolve(), Path(args.report).resolve()
    if build.exists() or report.exists() or build == report:
        parser.error('build/report must be distinct new directories; prior evidence is retained')
    requested = args.only or list(GROUPS)
    selected = list(dict.fromkeys(requested))
    # Public-header external linking has its own release-mode compile check.
    if args.sanitize and 'test-headers' in selected:
        selected = [g for g in selected if g != 'test-headers']
    if not selected:
        parser.error('no runnable groups: test-headers requires a release-mode run')
    build.mkdir(parents=True)
    report.mkdir(parents=True)
    signal.signal(signal.SIGTERM, interrupted)
    flags = '-std=c11 -Wall -Wextra -Wpedantic -O2 -g'
    if args.sanitize:
        flags = '-std=c11 -Wall -Wextra -Wpedantic -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined'
        if sys.platform.startswith('linux'):
            flags += ' -fno-pie -no-pie'
    record = dict(started_at=stamp(), platform=platform.platform(), machine=platform.machine(),
                  python=platform.python_version(), mode='sanitizer' if args.sanitize else 'release',
                  toolchain=toolchain(),
                  cflags=flags, requested_groups=requested, selected_groups=selected, inputs=inputs(), commands=[],
                  limits=['Deterministic fixtures only; no real-model or ESP32 hardware evidence.',
                          'Elapsed time covers each command; not model inference latency.',
                          'No memory or token efficiency claim follows from these acceptance runs.'])
    asan_options = 'detect_leaks={}:abort_on_error=1'.format(0 if sys.platform == 'darwin' else 1)
    record['sanitizer_options'] = dict(ASAN_OPTIONS=asan_options, UBSAN_OPTIONS='halt_on_error=1')
    if args.sanitize:
        record['limits'].append('Standalone external public-header link check is run in release mode only.')
        if sys.platform == 'darwin':
            record['limits'].append('Apple AddressSanitizer does not support LeakSanitizer; leak detection is enabled in the Linux runs only.')
    with tempfile.TemporaryDirectory(prefix='dunlin-agent-acceptance-') as isolated:
        env = dict(os.environ)
        for key in ('MAKEFLAGS', 'MFLAGS', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'PYTHONOPTIMIZE'):
            env.pop(key, None)
        env.update(HOME=isolated, XDG_CONFIG_HOME=isolated, TMPDIR=isolated,
                   PYTHONDONTWRITEBYTECODE='1', LC_ALL='C.UTF-8', CFLAGS=flags,
                   ASAN_OPTIONS=asan_options, UBSAN_OPTIONS='halt_on_error=1')
        for group in selected:
            command = ['make', '-j2', 'BUILD=' + str(build), 'CFLAGS=' + flags, group]
            log = report / (group + '.log')
            print('RUN ' + group, flush=True)
            start = time.monotonic()
            entry = dict(group=group, command=command, started_at=stamp(), log=log.name)
            try:
                with log.open('wb') as output:
                    process = subprocess.Popen(command, cwd=str(ROOT), env=env, stdout=output,
                                               stderr=subprocess.STDOUT, start_new_session=True)
                    try:
                        process.wait(timeout=600)
                    except (subprocess.TimeoutExpired, KeyboardInterrupt):
                        terminate_group(process)
                        raise
                entry['exit_code'] = process.returncode
                entry['status'] = 'pass' if process.returncode == 0 else 'fail'
            except (OSError, subprocess.TimeoutExpired) as error:
                entry.update(status='fail', error=str(error))
            except KeyboardInterrupt as error:
                entry.update(status='fail', error=str(error) or 'interrupted')
                record['interrupted'] = True
            entry.update(elapsed_seconds=round(time.monotonic() - start, 3), sha256=digest(log))
            record['commands'].append(entry)
            (report / 'results.json').write_text(json.dumps(record, indent=2) + '\n')
            print('{} {} ({:.3f}s)'.format(entry['status'].upper(), group, entry['elapsed_seconds']), flush=True)
            if record.get('interrupted'):
                break
    after = inputs()
    record['inputs_unchanged'] = record['inputs'] == after
    if not record['inputs_unchanged']:
        record['changed_inputs'] = sorted(k for k in set(after) | set(record['inputs'])
                                          if after.get(k) != record['inputs'].get(k))
    record['artifacts'] = {str(p.relative_to(build)): digest(p) for p in build.rglob('*')
                           if p.is_file() and (p.suffix == '.a' or p.name == 'dunlin' or p.name.endswith('-test'))}
    record['finished_at'] = stamp()
    record['status'] = 'pass' if (record['inputs_unchanged'] and not record.get('interrupted')
                                and len(record['commands']) == len(selected)
                                and all(c['status'] == 'pass' for c in record['commands'])) else 'fail'
    (report / 'results.json').write_text(json.dumps(record, indent=2) + '\n')
    print('{}: {}'.format(record['status'].upper(), report / 'results.json'), flush=True)
    return 130 if record.get('interrupted') else 0 if record['status'] == 'pass' else 1


if __name__ == '__main__':
    sys.exit(main())
