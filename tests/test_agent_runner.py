"""Verify acceptance orchestration rejects failures, stale inputs and overwrites."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest


class RunnerContract(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='dunlin-runner-test-')
        self.root = Path(self.tmp.name)
        (self.root / 'scripts').mkdir()
        (self.root / 'bin').mkdir()
        (self.root / 'Makefile').write_text('# isolated runner fixture\n')
        shutil.copyfile(Path(__file__).resolve().parents[1] / 'scripts/test-agent.py',
                        self.root / 'scripts/test-agent.py')
        self.make = self.root / 'bin/make'
        self.env = dict(os.environ, PATH=str(self.make.parent) + os.pathsep + os.environ['PATH'])

    def tearDown(self):
        self.tmp.cleanup()

    def run_fixture(self, body):
        self.make.write_text('#!/bin/sh\n' + body + '\n')
        self.make.chmod(0o700)
        command = [sys.executable, '-B', str(self.root / 'scripts/test-agent.py'),
                   '--build', str(self.root / 'build'), '--report', str(self.root / 'report'),
                   '--only', 'test-agent-contracts']
        result = subprocess.run(command, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return result, command

    def test_empty_sanitizer_selection_rejected(self):
        command = [sys.executable, '-B', str(self.root / 'scripts/test-agent.py'),
                   '--build', str(self.root / 'build'), '--report', str(self.root / 'report'),
                   '--sanitize', '--only', 'test-headers']
        result = subprocess.run(command, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(result.returncode, 2)
        self.assertFalse((self.root / 'report').exists())

    def test_interruption_reaps_owned_child(self):
        self.make.write_text('#!/bin/sh\necho $$ > child.pid\nexec sleep 30\n')
        self.make.chmod(0o700)
        command = [sys.executable, '-B', str(self.root / 'scripts/test-agent.py'),
                   '--build', str(self.root / 'build'), '--report', str(self.root / 'report'),
                   '--only', 'test-agent-contracts']
        proc = subprocess.Popen(command, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        child = None
        try:
            deadline = time.monotonic() + 5
            while not (self.root / 'child.pid').exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            child = int((self.root / 'child.pid').read_text())
            proc.send_signal(signal.SIGTERM)
            out, err = proc.communicate(timeout=10)
            self.assertEqual(proc.returncode, 130, (out, err))
            with self.assertRaises(ProcessLookupError):
                os.kill(child, 0)
            report = json.loads((self.root / 'report/results.json').read_text())
            self.assertEqual(report['status'], 'fail')
            self.assertTrue(report['interrupted'])
        finally:
            if proc.poll() is None:
                proc.kill(); proc.wait()
            if child:
                try:
                    os.kill(child, signal.SIGKILL)
                except ProcessLookupError:
                    pass

    def test_nonzero_exit_is_failure(self):
        result, _ = self.run_fixture('echo intentional-fixture-failure; exit 3')
        self.assertEqual(result.returncode, 1)
        report = json.loads((self.root / 'report/results.json').read_text())
        self.assertEqual(report['status'], 'fail')
        self.assertEqual(report['commands'][0]['exit_code'], 3)
        self.assertIn('intentional-fixture-failure', (self.root / 'report/test-agent-contracts.log').read_text())

    def test_changed_input_invalidates_success(self):
        result, _ = self.run_fixture("echo '# fixture mutation' >> Makefile; exit 0")
        self.assertEqual(result.returncode, 1)
        report = json.loads((self.root / 'report/results.json').read_text())
        self.assertEqual(report['commands'][0]['status'], 'pass')
        self.assertFalse(report['inputs_unchanged'])
        self.assertEqual(report['changed_inputs'], ['Makefile'])

    def test_success_isolated_and_cannot_overwrite(self):
        result, command = self.run_fixture('test "$HOME" = "$TMPDIR" && test -d "$HOME"')
        self.assertEqual(result.returncode, 0, result.stderr)
        original = (self.root / 'report/results.json').read_bytes()
        result = subprocess.run(command, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(result.returncode, 2)
        self.assertEqual((self.root / 'report/results.json').read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
