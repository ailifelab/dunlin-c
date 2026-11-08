#!/usr/bin/env python3
"""Measure only the protocol-fixture process, separately from payload/usage data.

No model, network, container, GPU or ESP32 memory is measured. Workloads contain
different positive/negative fixtures, so values are not a protocol efficiency rank.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', required=True, help='existing release build with protocol-test')
    p.add_argument('--report', required=True, help='new JSON result path')
    p.add_argument('--samples', type=int, default=10)
    args = p.parse_args()
    if not 2 <= args.samples <= 1000:
        p.error('samples must be 2..1000')
    if sys.platform != 'darwin' and not sys.platform.startswith('linux'):
        p.error('wait4 ru_maxrss units are only defined here for macOS and Linux')
    root = Path(__file__).resolve().parent.parent
    build, report = Path(args.build).resolve(), Path(args.report).resolve()
    binary, driver = build / 'protocol-test', build / 'protocol-measure'
    if not binary.is_file() or driver.exists() or report.exists():
        p.error('protocol-test must exist; driver and report must be new')
    command = ['cc', '-O2', str(root / 'tests/measure.c'), '-o', str(driver)]
    subprocess.run(command, check=True)
    record = dict(platform=platform.platform(), machine=platform.machine(), samples=args.samples,
                  warmup_samples=1, binary_sha256=sha(binary), driver_sha256=sha(driver),
                  driver_source_sha256=sha(root / 'tests/measure.c'), compile_command=command,
                  scope='wait4 child protocol-test peak RSS and spawn-to-exit wall time, including its loaded libraries; deterministic in-process fake transport only',
                  excluded=['CLI user workflow', 'real HTTP/TLS network', 'model service', 'container/VM memory', 'GPU', 'host total memory', 'ESP32 hardware'],
                  limits='Different fixture sets and process startup costs; not an equal-task protocol efficiency comparison. RSS is neither heap allocation high-water nor whole-system memory. No token estimates.',
                  results=[])
    with tempfile.TemporaryDirectory(prefix='dunlin-protocol-measure-') as home:
        env = dict(os.environ, HOME=home, TMPDIR=home)
        for protocol in ('chat', 'responses', 'anthropic', 'ollama'):
            command = [str(driver), str(args.samples), str(binary), protocol]
            result = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
            if result.returncode:
                record['failure'] = dict(command=command, exit_code=result.returncode,
                                         stderr=result.stderr.decode('utf-8', errors='replace'))
                break
            rows = [[float(line.split()[0]), int(line.split()[1])] for line in result.stdout.decode().splitlines()]
            if len(rows) != args.samples:
                raise RuntimeError('measurement driver returned an unexpected sample count')
            peak_bytes = [row[1] if sys.platform == 'darwin' else row[1] * 1024 for row in rows]
            record['results'].append(dict(protocol=protocol, command=command, exit_code=0,
                wall_ms=[row[0] for row in rows], peak_rss_bytes=peak_bytes,
                median_wall_ms=statistics.median(row[0] for row in rows), max_peak_rss_bytes=max(peak_bytes)))
    record['status'] = 'fail' if 'failure' in record else 'pass'
    with report.open('x') as output:
        json.dump(record, output, indent=2); output.write('\n')
    print(record['status'].upper() + ': ' + str(report))
    return 0 if record['status'] == 'pass' else 1


if __name__ == '__main__':
    sys.exit(main())
