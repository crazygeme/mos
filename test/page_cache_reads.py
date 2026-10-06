#!/usr/bin/env python3
"""Validate concurrent file-backed faults and descriptor offset preservation."""
import argparse
import concurrent.futures
import ctypes
import mmap
import os
from pathlib import Path
import random
import struct
import tempfile
import threading

PAGE = 4096
PAGES = 4096
WORKERS = 8
libc = ctypes.CDLL(None)
libc.memcmp.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
libc.memcmp.restype = ctypes.c_int


def page_content(index):
    word = struct.pack("<QQ", index, index ^ 0x9E3779B97F4A7C15)
    return word * (PAGE // len(word))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", default=".", help="Directory for the mapped regular file")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="page-cache-", dir=args.directory) as directory:
        path = Path(directory) / "pages"
        with path.open("wb") as stream:
            for index in range(PAGES):
                stream.write(page_content(index))
        fd = os.open(path, os.O_RDONLY)
        try:
            offset = 0x1237
            assert os.lseek(fd, offset, os.SEEK_SET) == offset
            with mmap.mmap(fd, PAGE * PAGES, access=mmap.ACCESS_COPY) as view:
                address = ctypes.addressof(ctypes.c_char.from_buffer(view))
                barrier = threading.Barrier(WORKERS)

                def check_pages(worker):
                    indices = list(range(worker, PAGES, WORKERS))
                    random.Random(worker).shuffle(indices)
                    barrier.wait(timeout=15)
                    for index in indices:
                        expected = ctypes.create_string_buffer(page_content(index))
                        # CDLL calls release the interpreter lock during the memory read.
                        if libc.memcmp(address + index * PAGE, expected, PAGE):
                            raise AssertionError(f"Mapped page {index} contains incorrect file data")

                with concurrent.futures.ThreadPoolExecutor(max_workers=WORKERS) as executor:
                    futures = [executor.submit(check_pages, worker) for worker in range(WORKERS)]
                    for future in futures:
                        future.result(timeout=60)
            assert os.lseek(fd, 0, os.SEEK_CUR) == offset, "Page faults changed the descriptor offset"
        finally:
            os.close(fd)
    print("Concurrent page-cache read checks: PASS")


if __name__ == "__main__":
    main()
