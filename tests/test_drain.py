"""Owned broker source controls; no installed sessions or coding clients."""
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

CLI = str(Path(sys.argv.pop(1)).resolve())
PRODUCER = r'''
import json,os,signal,sys,time
from pathlib import Path
root=Path(sys.argv[1]); mode=sys.argv[2]
def mark(name):
 fields=Path('/proc/self/stat').read_text().rsplit(') ',1)[1].split()
 (root/name).write_text(json.dumps({'pid':os.getpid(),'start':int(fields[19])}))
def writer():
 signal.signal(signal.SIGTERM,signal.SIG_IGN)
 os.setsid()
 if mode=='double' and os.fork(): os._exit(0)
 mark('writer.json')
 until=time.monotonic()+20
 while time.monotonic()<until:
  with (root/'writes').open('a') as out: out.write('x');out.flush()
  time.sleep(.01)
 os._exit(0)
def late(signum,frame):
 signal.signal(signal.SIGTERM,signal.SIG_IGN)
 if not os.fork(): writer()
 os._exit(0)
signal.signal(signal.SIGTERM,late if mode=='late' else signal.SIG_IGN)
mark('ready.json')
until=time.monotonic()+20
while time.monotonic()<until:
 if mode in ('detached','double') and (root/'fork-now').exists():
  if not os.fork(): writer()
  break
 time.sleep(.01)
while time.monotonic()<until: time.sleep(.01)
'''


def wait_file(path):
    until = time.monotonic() + 5
    while time.monotonic() < until:
        try:
            return json.loads(path.read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            time.sleep(.01)
    raise AssertionError('owned fixture did not publish ' + path.name)


@unittest.skipUnless(sys.platform.startswith('linux') and hasattr(os, 'pidfd_open'), 'Linux pidfds required')
class DrainTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='kpbdrain-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.rt = self.root / 'rt'
        self.rt.mkdir(mode=0o700)
        self.env = {'PATH': '/usr/bin:/bin', 'HOME': str(self.root), 'LANG': 'C.UTF-8'}
        self.pidfds = []
        self.broker = None
        self.addCleanup(self.cleanup_owned)

    def cli(self, *args):
        return subprocess.run([CLI, '--runtime-dir', str(self.rt), *args],
                              env=self.env, stdin=subprocess.DEVNULL,
                              capture_output=True, timeout=6)

    def capture(self, record):
        pid, started = record['pid'], record['start']
        def current():
            return int(Path('/proc', str(pid), 'stat').read_text().rsplit(') ', 1)[1].split()[19])
        try:
            if current() != started:
                return None
            fd = os.pidfd_open(pid)
            if current() != started:
                os.close(fd)
                return None
        except ProcessLookupError:
            return None
        except FileNotFoundError:
            return None
        self.pidfds.append(fd)
        return fd

    def cleanup_owned(self):
        # Discover only identities published in this fixture's private files.
        for name in ('writer.json', 'ready.json'):
            if (self.root / name).exists():
                self.capture(json.loads((self.root / name).read_text()))
        for fd in self.pidfds:
            try:
                signal.pidfd_send_signal(fd, signal.SIGKILL)
            except ProcessLookupError:
                pass
        for fd in self.pidfds:
            assert select.select([fd], [], [], 5)[0], 'owned process cessation unconfirmed'
            os.close(fd)
        if self.broker is not None:
            try:
                signal.pidfd_send_signal(self.broker, signal.SIGKILL)
            except ProcessLookupError:
                pass
            assert select.select([self.broker], [], [], 5)[0], 'owned broker cessation unconfirmed'
            os.close(self.broker)

    def spawn(self, mode):
        result = self.cli('run', '--id', 'owned', '--', sys.executable, '-c', PRODUCER, str(self.root), mode)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.cli('status', 'owned', '--json')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.status = json.loads(result.stdout)
        self.broker = os.pidfd_open(self.status['broker_pid'])
        self.initial = self.capture(wait_file(self.root / 'ready.json'))
        self.assertIsNotNone(self.initial)
        return self.status['started_millis']

    def drain(self, started, *options):
        return self.cli(*options, 'drain', 'owned', '--expect-started', str(started))

    def assert_writer_stopped(self):
        fd = self.capture(wait_file(self.root / 'writer.json'))
        if fd is not None:
            self.assertTrue(select.select([fd], [], [], 0)[0], 'ACK preceded writer cessation')
        before = (self.root / 'writes').stat().st_size
        time.sleep(.05)
        self.assertEqual((self.root / 'writes').stat().st_size, before)

    def test_late_detached_child_after_snapshot(self):
        started = self.spawn('detached')
        (self.root / 'fork-now').touch()
        self.capture(wait_file(self.root / 'writer.json'))
        result = self.drain(started)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(select.select([self.initial], [], [], 0)[0])
        self.assert_writer_stopped()

    def test_double_fork_descendant(self):
        started = self.spawn('double')
        (self.root / 'fork-now').touch()
        self.capture(wait_file(self.root / 'writer.json'))
        result = self.drain(started)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_writer_stopped()

    def test_child_created_by_termination_handler(self):
        started = self.spawn('late')
        result = self.drain(started)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_writer_stopped()

    def test_wrong_identity_does_not_terminate(self):
        started = self.spawn('none')
        self.assertEqual(self.drain(started + 1).returncode, 3)
        self.assertFalse(select.select([self.initial], [], [], .05)[0])
        self.assertEqual(self.drain(started).returncode, 0)

    def test_caller_timeout_does_not_release_descendants(self):
        started = self.spawn('late')
        result = self.drain(started, '--timeout', '0.1')
        self.assertNotEqual(result.returncode, 0)
        writer = self.capture(wait_file(self.root / 'writer.json'))
        if writer is not None:
            self.assertTrue(select.select([writer], [], [], 5)[0])
        self.assertTrue(select.select([self.initial], [], [], 5)[0])
        self.assert_writer_stopped()

    def test_legacy_kill_remains_request_acknowledgment(self):
        started = self.spawn('none')
        self.assertEqual(self.cli('kill', 'owned').returncode, 0)
        self.assertFalse(select.select([self.initial], [], [], 0)[0])
        self.assertEqual(self.drain(started).returncode, 0)

    def test_legacy_peer_refuses_new_frame_without_fallback(self):
        directory = self.rt / 'sessions' / 'owned'
        directory.mkdir(parents=True, mode=0o700)
        listener = socket.socket(socket.AF_UNIX)
        self.addCleanup(listener.close)
        listener.bind(str(directory / 'control.sock'))
        listener.listen(1)
        listener.settimeout(3)
        frames = []
        errors = []
        def peer():
            try:
                conn, _ = listener.accept()
                with conn:
                    conn.settimeout(3)
                    data = b''
                    while len(data) < 20:
                        part = conn.recv(20 - len(data))
                        if not part: raise AssertionError('truncated request')
                        data += part
                    frames.append(struct.unpack('!IHHIQ', data))
                    payload = b'invalid request'
                    conn.sendall(struct.pack('!IHHI', 0x4b504231, 1, 10, len(payload)) + payload)
                listener.settimeout(.2)
                try:
                    extra, _ = listener.accept()
                    extra.close()
                    errors.append('unexpected fallback connection')
                except socket.timeout:
                    pass
            except Exception as error:
                errors.append(str(error))
        thread = threading.Thread(target=peer)
        thread.start()
        result = self.drain(123)
        thread.join(4)
        self.assertFalse(thread.is_alive())
        self.assertEqual(result.returncode, 5, result.stderr)
        self.assertEqual(frames, [(0x4b504231, 1, 16, 8, 123)])
        self.assertFalse(errors, errors)

    def test_drain_requires_explicit_identity(self):
        self.assertEqual(self.cli('drain', 'owned').returncode, 2)
        self.assertEqual(self.cli('drain', 'owned', '--expect-started', 'bad').returncode, 2)


ORPHANS = r'''
import os,sys,time
from pathlib import Path
root=Path(sys.argv[1]); count=int(sys.argv[2])
for index in range(count):
 middle=os.fork()
 if middle==0:
  if os.fork()==0:
   time.sleep(.2)
   (root/('orphan-%d'%index)).write_text('%d %d'%(os.getpid(),os.getppid()))
   os._exit(0)
  os._exit(0)
 os.waitpid(middle,0)
(root/'command.pid').write_text(str(os.getpid()))
time.sleep(30)
'''


@unittest.skipUnless(sys.platform.startswith('linux') and hasattr(os, 'pidfd_open'), 'Linux pidfds required')
class AdoptedDescendantTests(unittest.TestCase):
    """A live session reaps the descendants its subreaper boundary adopts."""
    COUNT = 16

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='kpbreap-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.rt = self.root / 'rt'
        self.rt.mkdir(mode=0o700)
        self.env = {'PATH': '/usr/bin:/bin', 'HOME': str(self.root), 'LANG': 'C.UTF-8'}
        self.broker = None
        self.addCleanup(self.stop)

    def cli(self, *args):
        return subprocess.run([CLI, '--runtime-dir', str(self.rt), *args],
                              env=self.env, stdin=subprocess.DEVNULL,
                              capture_output=True, timeout=6)

    def stop(self):
        if self.broker is None:
            return
        self.cli('kill', 'orphans')
        if not select.select([self.broker], [], [], 5)[0]:
            signal.pidfd_send_signal(self.broker, signal.SIGKILL)
            assert select.select([self.broker], [], [], 5)[0], 'owned broker cessation unconfirmed'
        os.close(self.broker)

    @staticmethod
    def state(pid):
        try:
            return Path('/proc', str(pid), 'stat').read_text().rsplit(') ', 1)[1].split()[0]
        except FileNotFoundError:
            return None

    def zombies(self, broker_pid):
        path = Path('/proc', str(broker_pid), 'task', str(broker_pid), 'children')
        return [pid for pid in map(int, path.read_text().split()) if self.state(pid) == 'Z']

    def test_adopted_descendants_are_reaped_while_the_session_lives(self):
        result = self.cli('run', '--id', 'orphans', '--', sys.executable, '-c', ORPHANS,
                          str(self.root), str(self.COUNT))
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.cli('status', 'orphans', '--json')
        self.assertEqual(result.returncode, 0, result.stderr)
        broker_pid = json.loads(result.stdout)['broker_pid']
        self.broker = os.pidfd_open(broker_pid)
        until = time.monotonic() + 5
        orphans = [self.root / ('orphan-%d' % index) for index in range(self.COUNT)]
        while time.monotonic() < until and not all(path.exists() for path in orphans):
            time.sleep(.02)
        records = [tuple(map(int, path.read_text().split())) for path in orphans]
        # Control: every orphan really was adopted by this broker, not by init.
        self.assertEqual({parent for _, parent in records}, {broker_pid})
        command = int((self.root / 'command.pid').read_text())
        # Each orphan exits right after writing its record. Reaped means gone from
        # /proc altogether; a broker that only waits for its command leaves them 'Z'.
        until = time.monotonic() + 3
        while time.monotonic() < until and any(self.state(pid) for pid, _ in records):
            time.sleep(.02)
        self.assertEqual({pid: self.state(pid) for pid, _ in records if self.state(pid)}, {})
        self.assertEqual(self.zombies(broker_pid), [])
        # Reaping adopted descendants leaves the original command running.
        self.assertIn(self.state(command), ('S', 'R'))
        self.assertEqual(self.cli('status', 'orphans').returncode, 0)


if __name__ == '__main__':
    unittest.main()
