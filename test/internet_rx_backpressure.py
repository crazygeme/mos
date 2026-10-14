#!/usr/bin/env python3
"""Validate NIC receive progress during TCP receive backpressure.

Run --serve on the peer and --host on the guest. The peer must be reached
through the physical NIC rather than the loopback interface.
"""
import argparse
import hashlib
import json
import os
import socket
import time

PAYLOAD = bytes(range(256)) * 65536
DIGEST = hashlib.sha256(PAYLOAD).hexdigest()


def serve(address, port):
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((address, port))
        listener.listen(16)
        while True:
            connection, _ = listener.accept()
            if os.fork():
                connection.close()
                while True:
                    try:
                        if not os.waitpid(-1, os.WNOHANG)[0]:
                            break
                    except ChildProcessError:
                        break
                continue
            listener.close()
            try:
                with connection:
                    connection.settimeout(60)
                    request = connection.recv(1)
                    connection.sendall(PAYLOAD if request == b'D' else b'NETWORK_OK')
            except OSError:
                os._exit(1)
            os._exit(0)


def check(address, port):
    for trial in range(3):
        start = time.monotonic()
        with socket.create_connection((address, port), 10) as stream:
            stream.settimeout(30)
            stream.sendall(b'D')
            time.sleep(2)
            with socket.create_connection((address, port), 5) as probe:
                probe.settimeout(5)
                probe.sendall(b'P')
                reply = b''
                while len(reply) < len(b'NETWORK_OK'):
                    part = probe.recv(64)
                    assert part, 'Probe connection closed before response'
                    reply += part
                assert reply == b'NETWORK_OK', reply
            digest = hashlib.sha256()
            received = 0
            while True:
                data = stream.recv(65536)
                if not data:
                    break
                digest.update(data)
                received += len(data)
        assert received == len(PAYLOAD), received
        assert digest.hexdigest() == DIGEST, digest.hexdigest()
        print(json.dumps({'trial': trial + 1, 'bytes': received,
                          'sha256': digest.hexdigest(),
                          'parallel_connection': 'passed',
                          'seconds': round(time.monotonic() - start, 3)}), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--serve', metavar='ADDRESS', help='Peer listening address')
    mode.add_argument('--host', metavar='ADDRESS', help='Peer address reached by the guest')
    parser.add_argument('--port', type=int, default=18080)
    args = parser.parse_args()
    (serve if args.serve else check)(args.serve or args.host, args.port)
