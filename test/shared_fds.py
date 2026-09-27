#!/usr/bin/env python3
"""Exercise shared descriptor tables, fork isolation, and inotify lifetime."""
import ctypes
import errno
import fcntl
import os
import select
import threading
import unittest


class SharedDescriptors(unittest.TestCase):
    def run_thread(self, action):
        errors = []

        def run():
            try:
                action()
            except BaseException as exc:
                errors.append(exc)

        thread = threading.Thread(target=run)
        thread.start()
        thread.join(10)
        self.assertFalse(thread.is_alive(), 'descriptor worker timed out')
        if errors:
            raise errors[0]

    def test_worker_created_pipe_survives_thread_exit(self):
        descriptors = []

        def create():
            descriptors.extend(os.pipe())
            os.write(descriptors[1], b'worker')

        self.run_thread(create)
        read_fd, write_fd = descriptors
        try:
            self.assertEqual(os.read(read_fd, 6), b'worker')
            os.write(write_fd, b'main')
            self.assertEqual(os.read(read_fd, 4), b'main')
        finally:
            os.close(read_fd)
            os.close(write_fd)

    def test_main_created_fd_visible_to_existing_thread(self):
        ready = threading.Event()
        created = threading.Event()
        descriptors = []
        errors = []

        def consume():
            ready.set()
            if not created.wait(5):
                errors.append(AssertionError('pipe creation timed out'))
                return
            try:
                self.assertEqual(os.read(descriptors[0], 4), b'data')
            except BaseException as exc:
                errors.append(exc)

        thread = threading.Thread(target=consume)
        thread.start()
        self.assertTrue(ready.wait(5))
        descriptors.extend(os.pipe())
        try:
            os.write(descriptors[1], b'data')
            created.set()
            thread.join(10)
            self.assertFalse(thread.is_alive(), 'pipe reader timed out')
            if errors:
                raise errors[0]
        finally:
            for fd in descriptors:
                os.close(fd)

    def test_cloexec_and_close_are_shared(self):
        fd = os.open('/dev/null', os.O_RDONLY)
        closed = False
        try:
            fcntl.fcntl(fd, fcntl.F_SETFD, 0)
            self.run_thread(lambda: fcntl.fcntl(fd, fcntl.F_SETFD,
                                              fcntl.FD_CLOEXEC))
            self.assertEqual(fcntl.fcntl(fd, fcntl.F_GETFD), fcntl.FD_CLOEXEC)
            self.run_thread(lambda: os.close(fd))
            closed = True
            with self.assertRaises(OSError) as result:
                fcntl.fcntl(fd, fcntl.F_GETFD)
            self.assertEqual(result.exception.errno, errno.EBADF)
        finally:
            if not closed:
                os.close(fd)

    def test_fork_close_is_private(self):
        fd = os.open('/dev/null', os.O_RDONLY)
        try:
            pid = os.fork()
            if pid == 0:
                os.close(fd)
                os._exit(0)
            self.assertEqual(os.waitpid(pid, 0)[1], 0)
            self.assertEqual(os.read(fd, 1), b'')
        finally:
            os.close(fd)

    def test_exec_unshares_clone_files(self):
        libc = ctypes.CDLL(None, use_errno=True)
        callback_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
        libc.clone.argtypes = [callback_type, ctypes.c_void_p,
                               ctypes.c_int, ctypes.c_void_p]
        libc.clone.restype = ctypes.c_int
        stack = ctypes.create_string_buffer(256 * 1024)
        stack_top = (ctypes.addressof(stack) + len(stack)) & ~15
        fd = os.open('/dev/null', os.O_RDONLY | os.O_CLOEXEC)

        @callback_type
        def child(_):
            os.execl('/bin/true', 'true')
            return 127

        try:
            # CLONE_FILES | SIGCHLD, with a separate address space.
            pid = libc.clone(child, stack_top, 0x400 | 17, None)
            self.assertGreater(pid, 0, os.strerror(ctypes.get_errno()))
            self.assertEqual(os.waitpid(pid, 0)[1], 0)
            self.assertEqual(os.read(fd, 1), b'')
            self.assertEqual(fcntl.fcntl(fd, fcntl.F_GETFD), fcntl.FD_CLOEXEC)
        finally:
            os.close(fd)

    def test_inotify_survives_child_close_and_exec(self):
        libc = ctypes.CDLL(None, use_errno=True)
        libc.inotify_init1.argtypes = [ctypes.c_int]
        libc.inotify_init1.restype = ctypes.c_int
        fd = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
        self.assertGreaterEqual(fd, 0, os.strerror(ctypes.get_errno()))
        try:
            for exec_child in (False, True):
                pid = os.fork()
                if pid == 0:
                    if exec_child:
                        os.execl('/bin/true', 'true')
                    os.close(fd)
                    os._exit(0)
                self.assertEqual(os.waitpid(pid, 0)[1], 0)
                temporary = [os.open('/dev/null', os.O_RDONLY)
                             for _ in range(32)]
                try:
                    poller = select.poll()
                    poller.register(fd, select.POLLIN)
                    self.assertEqual(poller.poll(0), [])
                    with self.assertRaises(OSError) as result:
                        os.read(fd, 4096)
                    self.assertEqual(result.exception.errno, errno.EAGAIN)
                finally:
                    for other in temporary:
                        os.close(other)
        finally:
            os.close(fd)


if __name__ == '__main__':
    unittest.main(verbosity=2)
