"""Private authenticated transfer adapter for the in-game join bridge.

No windows or launch/restart controls. The running game receives verification
through the versioned local bridge contract and loads its own snapshot.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shlex
import socket
import subprocess
import time


class Cancelled(Exception):
    pass


class Service:
    def __init__(self, config):
        self.config = config
        self.directory = Path(config['bridge_directory'])
        self.deadline = time.monotonic() + config.get('maximum_seconds', 1800)
        self.request = None
        self.connected = False

    def publish(self, phase, message, **extra):
        record = {'id': self.request['id'] if self.request else '', 'phase': phase,
                  'message': message, 'updated': time.time(), **extra}
        for name, value in [('status.json', json.dumps(record) + '\n'),
                            ('status.txt', '|'.join(str(record.get(k, '')) for k in
                             ('id', 'phase', 'message', 'endpoint', 'slot')))]:
            temporary = self.directory / (name + '.next')
            temporary.write_text(value, encoding='utf-8')
            temporary.replace(self.directory / name)
        print(phase + ': ' + message, flush=True)

    def cancelled(self):
        if time.monotonic() >= self.deadline:
            return True
        path = self.directory / 'cancel.json'
        if path.exists():
            claim = path.with_suffix('.claimed')
            path.replace(claim)
            return json.loads(claim.read_text())['id'] == self.request['id']
        return False

    def check(self):
        if self.cancelled():
            raise Cancelled()

    @staticmethod
    def remote(server):
        result = subprocess.run(server['ssh_command'] + [server['status_command']],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=10, check=True)
        return json.loads(result.stdout)

    @staticmethod
    def probe_transport(endpoint, port, identity):
        # A local SSH listener alone says nothing about its remote channel.
        # Require the actual native greeting; never send READY or game actions.
        with socket.create_connection((endpoint, port), timeout=3) as connection:
            connection.settimeout(3)
            connection.sendall(('JOIN x4mp preflight-' + identity + ' JoinCheck\n').encode('ascii'))
            response = b''
            while len(response) < 4096:
                chunk = connection.recv(4096 - len(response))
                if not chunk:
                    break
                response += chunk
                if re.search(rb'(?:^|\n)WELCOME [0-9]+\r?\n', response):
                    return
            raise OSError('native greeting unavailable')

    def download(self, server, manifest):
        remote = PurePosixPath(manifest['snapshot'])
        allowed = PurePosixPath(server['snapshot_root'])
        if '..' in remote.parts or not remote.is_relative_to(allowed) or remote.name != 'save_001.xml.gz':
            raise ValueError('snapshot path')
        size = int(manifest['bytes'])
        if not 0 < size <= 512 * 1024**2 or not re.fullmatch('[0-9a-f]{64}', manifest['sha256']):
            raise ValueError('snapshot manifest')
        target = Path(self.config['save_directory']) / 'save_001.xml.gz'
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_suffix('.incoming')
        started = time.monotonic()
        process = None
        try:
            with temporary.open('wb') as writer:
                process = subprocess.Popen(server['ssh_command'] + ['cat ' + shlex.quote(str(remote))],
                                           stdout=writer, stderr=subprocess.DEVNULL)
                while process.poll() is None:
                    self.check()
                    received = temporary.stat().st_size
                    if received > size or time.monotonic() - started > 120:
                        raise ValueError('transfer limit')
                    self.publish('syncing', f'Syncing universe snapshot: {received * 100 // size}%.')
                    time.sleep(.5)
                if process.returncode:
                    raise ValueError('transfer failed')
            self.check()
            self.publish('validating', 'Checking snapshot integrity and compatibility...')
            if temporary.stat().st_size != size:
                raise ValueError('snapshot length')
            digest = hashlib.sha256()
            with temporary.open('rb') as stream:
                while chunk := stream.read(1024**2):
                    self.check(); digest.update(chunk)
            if digest.hexdigest() != manifest['sha256']:
                raise ValueError('snapshot checksum')
            expanded = 0
            with gzip.open(temporary, 'rb') as stream:
                while chunk := stream.read(4 * 1024**2):
                    self.check(); expanded += len(chunk)
                    if expanded > 2 * 1024**3:
                        raise ValueError('snapshot expansion')
            self.check()
            if target.exists():
                target.replace(target.with_name(f'previous-{time.time_ns()}.xml.gz'))
            temporary.replace(target)
            return digest.hexdigest()
        finally:
            if process is not None and process.poll() is None:
                process.terminate(); process.wait(timeout=10)

    def join(self):
        self.connected = False
        server = next((s for s in self.config['servers'] if s['ip'] == self.request['ip']), None)
        if server is None:
            self.publish('error', 'No authenticated profile is configured for this address.'); return
        self.publish('requesting', 'Requesting to join ' + server['name'] + '...')
        while True:
            self.check()
            manifest = self.remote(server)
            if manifest['phase'] == 'stopped':
                self.publish('error', 'The bounded host is offline. Please retry when it is ready.'); return
            if manifest['phase'] == 'ready':
                break
            self.publish('waiting', 'Waiting for the host to prepare its universe snapshot...')
            time.sleep(2)
        if (manifest['game_version'] != self.config['game_version'] or
                sorted(manifest['dlc']) != sorted(self.config['dlc'])):
            self.publish('error', 'Game version or DLC does not match the server.'); return
        endpoint = server.get('connect_ip', self.config.get('connect_ip', '127.0.0.1'))
        port = int(self.config['connect_port'])
        self.publish('requesting', 'Checking the private host connection...')
        try:
            self.probe_transport(endpoint, port, self.request['id'])
        except OSError:
            self.publish('error', 'The host connection is unavailable. No universe was loaded.'); return
        self.check()
        digest = self.download(server, manifest)
        self.check()
        # The game loads the verified snapshot itself. Never restart X4 here.
        self.publish('verified', 'Snapshot verified. Preparing the universe...',
                     endpoint=endpoint, slot='save_001', snapshot_sha256=digest)
        native_log = Path(self.config['native_log'])
        log_offset = native_log.stat().st_size if native_log.exists() else 0
        while True:
            self.check()
            manifest = self.remote(server)
            if manifest['phase'] == 'stopped':
                self.publish('disconnected', 'The bounded host has stopped.'); return
            text = ''
            if native_log.exists():
                with native_log.open('rb') as log:
                    if native_log.stat().st_size < log_offset: log_offset = 0
                    log.seek(log_offset); text = log.read().decode(errors='replace')
            ready = 'ready to render host state (via universe_ready)' in text
            if ready and int(manifest.get('clients', 0)) > 0 and not self.connected:
                self.connected = True
                self.publish('connected', 'Connected to ' + server['name'] + '.',
                             snapshot_sha256=digest, trial=manifest.get('trial'))
            time.sleep(2)

    def run(self):
        catalog = '\n'.join(f"server-{i}|{s['name']}|{s['ip']}" for i, s in enumerate(self.config['servers']))
        if any('|' in s['name'] or '\n' in s['name'] for s in self.config['servers']):
            raise ValueError('invalid catalog')
        (self.directory / 'catalog.txt').write_text(catalog, encoding='utf-8')
        while time.monotonic() < self.deadline:
            path = self.directory / 'request.json'
            if not path.exists():
                time.sleep(.25); continue
            claim = path.with_suffix('.claimed')
            path.replace(claim)
            self.request = json.loads(claim.read_text(encoding='utf-8-sig'))
            if not re.fullmatch(r'[\w.-]{1,64}', self.request.get('id', ''), flags=re.ASCII):
                continue
            try:
                self.join()
            except Cancelled:
                self.publish('cancelled', 'Join cancelled.')
            except (OSError, ValueError, KeyError, subprocess.SubprocessError):
                # Keep machine paths/commands and authentication failures out of
                # the player UI and avoid presenting a failed transfer as ready.
                self.publish('error', 'Join service or snapshot verification failed. Please retry.')
        if self.request:
            self.publish('disconnected' if self.connected else 'error', 'The bounded join test has ended.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', type=Path, required=True)
    arguments = parser.parse_args()
    Service(json.loads(arguments.config.read_text(encoding='utf-8-sig'))).run()
