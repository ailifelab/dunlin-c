"""A model-fixture assertion must fail its controlling test, even in a worker."""
import unittest
import threading
from mock_model import MockServer


class FixtureFailures(unittest.TestCase):
    def test_delayed_worker_failure_reaches_controller(self):
        server = MockServer.__new__(MockServer)
        server.errors, server.workers = [], []
        ready, release = threading.Event(), threading.Event()
        def worker(request, address):
            ready.set()
            release.wait(1)
            try:
                raise AssertionError('delayed worker failure')
            except AssertionError:
                server.handle_error(request, address)
        server.process_request_thread = worker
        server.process_request(None, ('127.0.0.1', 0))
        self.assertTrue(ready.wait(1))
        server.assert_healthy()  # A pre-completion check alone is insufficient.
        release.set()
        with self.assertRaisesRegex(AssertionError, 'delayed worker failure'):
            server.join_workers()
        self.assertFalse(any(t.is_alive() for t in server.workers))

    def test_worker_failure_reaches_controller(self):
        # No socket is opened; exercise the HTTPServer error reporting boundary.
        server = MockServer.__new__(MockServer)
        server.errors = []
        server.assert_healthy()
        try:
            raise AssertionError('intentional request contract failure')
        except AssertionError:
            server.handle_error(None, ('127.0.0.1', 0))
        with self.assertRaisesRegex(AssertionError, 'intentional request contract failure'):
            server.assert_healthy()


if __name__ == '__main__':
    unittest.main()
