#!/usr/bin/env python3
"""Run the epoll guest test and benchmark with an isolated root filesystem."""
import argparse
import json
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import tempfile


def initramfs(probe, destination):
    entries = [(".", stat.S_IFDIR | 0o755, b""),
               ("dev", stat.S_IFDIR | 0o755, b""),
               ("proc", stat.S_IFDIR | 0o755, b""),
               ("tmp", stat.S_IFDIR | 0o1777, b""),
               ("dev/console", stat.S_IFCHR | 0o600, b""),
               ("dev/null", stat.S_IFCHR | 0o666, b""),
               ("init", stat.S_IFREG | 0o755, probe.read_bytes()),
               ("TRAILER!!!", 0, b"")]
    archive = bytearray()
    for ino, (name, mode, data) in enumerate(entries, 1):
        major, minor = ((5, 1) if name == "dev/console" else
                        (1, 3) if name == "dev/null" else (0, 0))
        fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0,
                  major, minor, len(name) + 1, 0]
        archive += b"070701" + "".join(f"{v:08x}" for v in fields).encode()
        archive += name.encode() + b"\0"
        archive += b"\0" * (-len(archive) % 4)
        archive += data
        archive += b"\0" * (-len(archive) % 4)
    destination.write_bytes(archive)


def disk_image(probe, directory):
    stage = directory / "root"
    for name in ("sbin", "bin", "dev", "proc", "tmp", "root", "etc"):
        (stage / name).mkdir(parents=True, exist_ok=True)
    shutil.copy2(probe, stage / "sbin/init")
    part = directory / "partition.img"
    with part.open("wb") as stream:
        stream.truncate(128 * 1024 * 1024)
    subprocess.run(["mke2fs", "-q", "-t", "ext3", "-b", "4096", "-F",
                    "-d", str(stage), str(part)], check=True)
    mbr = bytearray(512)
    mbr[446:462] = struct.pack("<B3sB3sII", 0x80, b"\0\x02\0", 0x83,
                              b"\xfe\xff\xff", 2048, 262144)
    mbr[510:512] = b"\x55\xaa"
    disk = directory / "disk.img"
    with disk.open("wb") as out, part.open("rb") as inp:
        out.write(mbr)
        out.seek(1048576)
        shutil.copyfileobj(inp, out)
    return disk


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True,
                        help="Statically linked epoll regression probe")
    parser.add_argument("--linux", action="store_true", help="Use a Linux initramfs")
    parser.add_argument("--cpus", type=int, default=2)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--baseline", type=Path, help="Matched Linux benchmark JSON")
    parser.add_argument("--max-slowdown", type=float, default=1.5)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="epoll-qemu-") as raw:
        directory = Path(raw)
        command = [args.qemu, "-enable-kvm", "-cpu", "host", "-smp", str(args.cpus),
                   "-m", "512", "-display", "none", "-serial", "stdio", "-monitor", "none",
                   "-kernel", str(args.kernel.resolve()), "-no-reboot", "-net", "none"]
        if args.linux:
            archive = directory / "initramfs.cpio"
            initramfs(args.probe, archive)
            command += ["-initrd", str(archive), "-append", "console=ttyS0 quiet panic=1 rdinit=/init"]
        else:
            disk = disk_image(args.probe, directory)
            command += ["-drive", f"file={disk},format=raw,if=ide", "-append", "verbose=0",
                        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
        try:
            run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, timeout=args.timeout)
        except subprocess.TimeoutExpired as error:
            output = error.stdout or b""
            args.output.write_text(output.decode(errors="replace") if isinstance(output, bytes) else output)
            raise SystemExit(f"Guest execution exceeded {args.timeout} seconds; output: {args.output}")
        args.output.write_text(run.stdout)
        rows = [{k: int(v) for k, v in match.groupdict().items()}
                for match in re.finditer(r"BENCH n=(?P<n>\d+) empty_ns=(?P<empty_ns>\d+) "
                                        r"ready_ns=(?P<ready_ns>\d+) poll_ns=(?P<poll_ns>\d+) "
                                        r"mod_ns=(?P<mod_ns>\d+)", run.stdout)]
        passed = ("EPOLL_PROBE_COMPLETE PASS" in run.stdout and len(rows) == 3 and
                  "FAIL" not in run.stdout and "[  FAILED  ]" not in run.stdout)
        ratios = []
        if args.baseline:
            baseline = json.loads(args.baseline.read_text())
            if not baseline["passed"] or baseline["cpus"] != args.cpus:
                raise SystemExit("The baseline must pass with the same CPU count")
            indexed = {row["n"]: row for row in baseline["benchmarks"]}
            for row in rows:
                comparison = {"n": row["n"]}
                for metric in ("empty_ns", "ready_ns", "mod_ns"):
                    ratio = row[metric] / indexed[row["n"]][metric]
                    comparison[metric] = round(ratio, 3)
                    if ratio > args.max_slowdown:
                        passed = False
                ratios.append(comparison)
        if rows:
            for metric in ("empty_ns", "ready_ns"):
                if rows[-1][metric] > rows[0][metric] * 1.5 + 50:
                    passed = False
        args.output.with_suffix(".json").write_text(json.dumps({
            "passed": passed, "kernel": str(args.kernel.resolve()),
            "probe": str(args.probe.resolve()), "cpus": args.cpus,
            "acceleration": "KVM", "benchmarks": rows, "ratios_to_linux": ratios}, indent=2) + "\n")
        for line in run.stdout.splitlines():
            if "PASS" in line or "BENCH" in line or "FAIL" in line:
                print(line)
        if not passed:
            raise SystemExit(f"Guest epoll validation failed; output: {args.output}")


if __name__ == "__main__":
    main()
