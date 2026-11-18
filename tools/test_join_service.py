"""Exercise snapshot validation and cancellation without a server or game."""
import gzip
import hashlib
from pathlib import Path
import sys
import socket
import threading
import tempfile
import unittest
from unittest.mock import patch

from join_service import Cancelled, Service


class SnapshotChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.save = self.directory / 'save'
        self.save.mkdir()
        self.target = self.save / 'save_001.xml.gz'
        self.target.write_bytes(b'preserved previous save')
        self.service = Service({'bridge_directory': str(self.directory), 'save_directory': str(self.save)})
        self.service.request = {'id': 'join-1', 'ip': 'example.net'}
        self.payload = gzip.compress(b'<savegame>disposable fixture</savegame>')
        self.server = {'snapshot_root': '/owned/trials', 'ssh_command': [sys.executable, '-c',
            'import sys; sys.stdout.buffer.write(' + repr(self.payload) + ')']}
        self.manifest = {'snapshot': '/owned/trials/test/save_001.xml.gz',
                         'bytes': len(self.payload), 'sha256': hashlib.sha256(self.payload).hexdigest()}

    def tearDown(self):
        self.temp.cleanup()

    def test_valid_snapshot_preserves_previous(self):
        self.assertEqual(self.service.download(self.server, self.manifest), self.manifest['sha256'])
        self.assertEqual(self.target.read_bytes(), self.payload)
        self.assertEqual(next(self.save.glob('previous-*')).read_bytes(), b'preserved previous save')

    def test_bad_hash_cannot_replace_save(self):
        self.manifest['sha256'] = '0' * 64
        with self.assertRaises(ValueError): self.service.download(self.server, self.manifest)
        self.assertEqual(self.target.read_bytes(), b'preserved previous save')

    def test_traversal_rejected_before_transfer(self):
        self.manifest['snapshot'] = '/owned/trials/../private/save_001.xml.gz'
        with self.assertRaises(ValueError): self.service.download(self.server, self.manifest)

    def test_cancel_cannot_promote_snapshot(self):
        (self.directory / 'cancel.json').write_text('{"id":"join-1"}')
        with self.assertRaises(Cancelled): self.service.download(self.server, self.manifest)
        self.assertEqual(self.target.read_bytes(), b'preserved previous save')

    def test_stale_cancel_does_not_cancel_current_request(self):
        (self.directory / 'cancel.json').write_text('{"id":"previous-request"}')
        self.assertFalse(self.service.cancelled())

    def test_oversized_transfer_cannot_replace_save(self):
        self.manifest['bytes'] -= 1
        with self.assertRaises(ValueError): self.service.download(self.server, self.manifest)
        self.assertEqual(self.target.read_bytes(), b'preserved previous save')

    def transport_fixture(self, reply):
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0)); listener.listen(1)
        port = listener.getsockname()[1]
        def serve():
            try:
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(3); connection.recv(1024)
                    if reply: connection.sendall(reply)
            finally: listener.close()
        thread = threading.Thread(target=serve, daemon=True); thread.start()
        try:
            Service.probe_transport('127.0.0.1', port, 'fixture')
        finally:
            thread.join(timeout=4)
            self.assertFalse(thread.is_alive())

    def test_native_greeting_required(self):
        self.transport_fixture(b'WELCOME 7\n')

    def test_local_listener_alone_is_not_a_working_host(self):
        with self.assertRaises(OSError): self.transport_fixture(b'')

    def test_denied_transport_never_downloads_or_publishes_verified(self):
        self.service.config.update(servers=[{'ip':'example.net','name':'Fixture'}],
                                   game_version='900', dlc=[], connect_port=12345)
        with patch.object(self.service, 'remote', return_value={'phase':'ready','game_version':'900','dlc':[]}), \
             patch.object(self.service, 'probe_transport', side_effect=OSError('denied')), \
             patch.object(self.service, 'download') as download:
            self.service.join()
            download.assert_not_called()
        self.assertIn('"phase": "error"', (self.directory/'status.json').read_text())
        self.assertEqual(self.target.read_bytes(), b'preserved previous save')


if __name__ == '__main__':
    unittest.main()
