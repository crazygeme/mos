#!/usr/bin/env python3
"""Verify F_DUPFD_CLOEXEC allocation, file sharing, and exec semantics."""
import errno
import fcntl
import os
import resource
import sys
import tempfile
import unittest

F_DUPFD_CLOEXEC = 1030


class DupfdCloexec(unittest.TestCase):
    def test_lowest_free_descriptor_and_shared_offset(self):
        with tempfile.TemporaryFile() as source:
            source.write(b'abcd')
            source.flush()
            source.seek(0)
            fd = source.fileno()
            fcntl.fcntl(fd, fcntl.F_SETFD, 0)
            occupied = fcntl.fcntl(fd, fcntl.F_DUPFD, 64)
            duplicate = -1
            try:
                duplicate = fcntl.fcntl(fd, F_DUPFD_CLOEXEC, occupied)
                self.assertGreater(duplicate, occupied)
                self.assertEqual(fcntl.fcntl(duplicate, fcntl.F_GETFD),
                                 fcntl.FD_CLOEXEC)
                self.assertEqual(fcntl.fcntl(fd, fcntl.F_GETFD), 0)
                self.assertEqual(fcntl.fcntl(occupied, fcntl.F_GETFD), 0)
                self.assertEqual(os.read(duplicate, 1), b'a')
                self.assertEqual(os.read(fd, 1), b'b')
                os.close(occupied)
                occupied = -1
                replacement = fcntl.fcntl(fd, F_DUPFD_CLOEXEC, 64)
                try:
                    self.assertLess(replacement, duplicate)
                finally:
                    os.close(replacement)
            finally:
                if occupied >= 0:
                    os.close(occupied)
                if duplicate >= 0:
                    os.close(duplicate)

    def test_exec_closes_duplicate(self):
        source = os.open('/dev/null', os.O_RDONLY | os.O_CLOEXEC)
        duplicate = -1
        try:
            duplicate = fcntl.fcntl(source, F_DUPFD_CLOEXEC, 64)
            pid = os.fork()
            if pid == 0:
                code = '''import errno, fcntl, sys
try:
    fcntl.fcntl(int(sys.argv[1]), fcntl.F_GETFD)
except OSError as exc:
    sys.exit(0 if exc.errno == errno.EBADF else 2)
sys.exit(1)
'''
                os.execl(sys.executable, sys.executable, '-c', code,
                         str(duplicate))
            self.assertEqual(os.waitpid(pid, 0)[1], 0)
            self.assertEqual(os.read(duplicate, 1), b'')
        finally:
            if duplicate >= 0:
                os.close(duplicate)
            os.close(source)

    def test_invalid_fd_and_minimum(self):
        with self.assertRaises(OSError) as result:
            fcntl.fcntl(0x7fffffff, F_DUPFD_CLOEXEC, 0)
        self.assertEqual(result.exception.errno, errno.EBADF)
        fd = os.open('/dev/null', os.O_RDONLY)
        try:
            for minimum in (-1, resource.getrlimit(resource.RLIMIT_NOFILE)[0]):
                with self.assertRaises(OSError) as result:
                    fcntl.fcntl(fd, F_DUPFD_CLOEXEC, minimum)
                self.assertEqual(result.exception.errno, errno.EINVAL)
        finally:
            os.close(fd)


if __name__ == '__main__':
    unittest.main(verbosity=2)
