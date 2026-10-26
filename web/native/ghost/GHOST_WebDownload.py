# SPDX-License-Identifier: GPL-2.0-or-later
"""Thread workers and copied message channels for Blender's HTTP downloader.

This implements the context operations used by that downloader. It does not
replace Python's multiprocessing module or create operating-system processes.
"""
from collections import deque
import copy
import pickle
import threading
import time
import weakref

EventClass = threading.Event


class _Channel:
    def __init__(self):
        self.condition = threading.Condition()
        self.messages = deque()
        self.writer_closed = False
        self.reader_closed = False


class _Endpoint:
    def __init__(self, incoming, outgoing):
        self.incoming, self.outgoing = incoming, outgoing
        self.closed = False

    def send(self, value):
        if self.closed:
            raise OSError('Downloader channel is closed')
        data = pickle.dumps(value, protocol=pickle.HIGHEST_PROTOCOL)
        channel = self.outgoing
        with channel.condition:
            if channel.reader_closed or channel.writer_closed:
                raise BrokenPipeError('Downloader channel peer is closed')
            channel.messages.append(data)
            channel.condition.notify_all()

    def poll(self, timeout=0):
        if self.closed:
            raise OSError('Downloader channel is closed')
        deadline = None if timeout is None else time.monotonic() + max(0, timeout)
        channel = self.incoming
        with channel.condition:
            while not channel.messages and not channel.writer_closed:
                if channel.reader_closed:
                    raise OSError('Downloader channel is closed')
                remaining = None if deadline is None else deadline - time.monotonic()
                if remaining is not None and remaining <= 0:
                    return False
                channel.condition.wait(remaining)
            return True

    def recv(self):
        if self.closed:
            raise OSError('Downloader channel is closed')
        channel = self.incoming
        with channel.condition:
            while not channel.messages:
                if channel.reader_closed:
                    raise OSError('Downloader channel is closed')
                if channel.writer_closed:
                    raise EOFError('Downloader channel peer is closed')
                channel.condition.wait()
            data = channel.messages.popleft()
        return pickle.loads(data)

    def close(self):
        self.closed = True
        with self.outgoing.condition:
            self.outgoing.writer_closed = True
            self.outgoing.condition.notify_all()
        with self.incoming.condition:
            self.incoming.reader_closed = True
            self.incoming.condition.notify_all()


class _DownloadContext:
    Event = threading.Event

    @staticmethod
    def Pipe(duplex=True):
        if not duplex:
            raise ValueError('Downloader channels require a duplex pipe')
        forward, backward = _Channel(), _Channel()
        return _Endpoint(backward, forward), _Endpoint(forward, backward)

    @staticmethod
    def Process(*, name, target, args, daemon):
        # Only the endpoint is shared. Preserve the downloader options snapshot
        # that the native spawn context obtains through serialization.
        endpoint, options = args
        snapshot = copy.deepcopy(options)

        def run():
            try:
                target(endpoint, snapshot)
            finally:
                endpoint.close()

        return threading.Thread(name=name, target=run, daemon=daemon)


def downloader_context():
    return _DownloadContext()


_file_locks = weakref.WeakValueDictionary()
_file_locks_guard = threading.Lock()


def mutex_lock_and_open(file_path, mode):
    """Protect metadata across threads before opening or truncating the file.

    OPFS access handles provide browser file ownership. This additional mutex
    coordinates workers sharing one interpreter and filesystem instance.
    """
    assert 'b' in mode
    key = str(file_path.resolve())
    with _file_locks_guard:
        lock = _file_locks.get(key)
        if lock is None:
            lock = threading.Lock()
            _file_locks[key] = lock
    if not lock.acquire(blocking=False):
        return None, None
    try:
        file = file_path.open(mode)
    except OSError:
        lock.release()
        return None, None

    def unlock_and_close(file):
        try:
            file.close()
        finally:
            lock.release()

    return file, unlock_and_close
