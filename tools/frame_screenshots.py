#!/usr/bin/env python3
"""Generate self-contained SVG screenshots with transparent drop shadows."""

import base64
from html import escape
from pathlib import Path
import struct

SCREENSHOTS = Path(__file__).resolve().parents[1] / "doc/screenshot"
TITLES = {
    "boot": "Kernel and SysV init startup",
    "browser": "Microsoft Edge · Baidu",
    "dns": "DNS resolution",
    "final": "MOS kernel statistics",
    "gui1": "Xfce desktop",
    "gui2": "Desktop applications",
    "handle_signal": "Shell signal handler",
    "login_prompt": "XDM login",
    "login_succ": "Authenticated desktop session",
    "ls": "Directory listings",
    "network": "Network configuration",
    "ping": "ICMP echo",
    "proc": "MOS procfs",
    "ps_all": "Process overview",
    "ps_session0": "System services",
    "ps_session1": "Desktop account processes",
    "send_signal": "Process signal delivery",
    "ssh_login": "SSH session",
    "throughput": "Network counters and ARP cache",
    "top": "Process activity",
    "version": "Kernel compatibility identity",
    "versions": "GNU userspace versions",
    "vim": "Vim · C source",
    "vscode": "Visual Studio Code · MOS source",
    "wget": "GNU Wget · HTTPS download",
    "x64_8g_desktop": "AMD64 desktop · Memory",
}


def frame(path: Path) -> str:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError(f"Invalid PNG screenshot: {path}")
    width, height = struct.unpack(">II", data[16:24])
    margin = max(18, round(width / 48))
    canvas_width = width + 2 * margin
    canvas_height = height + 2 * margin
    title = escape(TITLES[path.stem])
    payload = base64.b64encode(data).decode("ascii")
    return f'''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     width="{canvas_width}" height="{canvas_height}" viewBox="0 0 {canvas_width} {canvas_height}"
     role="img" aria-labelledby="title description">
  <title id="title">GNU/MOS — {title}</title>
  <desc id="description">Guest screenshot with a soft drop shadow on a transparent background.</desc>
  <defs>
    <filter id="shadow" x="-20%" y="-20%" width="140%" height="150%" color-interpolation-filters="sRGB">
      <feGaussianBlur in="SourceAlpha" stdDeviation="{margin / 4:.1f}"/>
      <feOffset dy="{margin / 5:.1f}"/>
      <feComponentTransfer><feFuncA type="linear" slope="0.24"/></feComponentTransfer>
      <feMerge><feMergeNode/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
  </defs>
  <image x="{margin}" y="{margin}" width="{width}" height="{height}"
         filter="url(#shadow)" xlink:href="data:image/png;base64,{payload}"/>
</svg>
'''


def main() -> None:
    output = SCREENSHOTS / "framed"
    output.mkdir(exist_ok=True)
    for path in sorted(SCREENSHOTS.glob("*.png")):
        (output / (path.stem + ".svg")).write_text(frame(path), encoding="utf-8")


if __name__ == "__main__":
    main()
