#!/usr/bin/env python3
"""Generate self-contained SVG screenshots with raised frames and layered shadows."""

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
    margin = max(32, round(width / 24))
    rim = max(3, round(width / 240))
    depth = max(5, round(width / 160))
    radius = rim * 2
    frame_x = margin - rim
    frame_y = margin - rim
    frame_width = width + rim * 2
    frame_height = height + rim * 2
    canvas_width = width + 2 * margin
    canvas_height = height + 2 * margin
    title = escape(TITLES[path.stem])
    payload = base64.b64encode(data).decode("ascii")
    return f'''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     width="{canvas_width}" height="{canvas_height}" viewBox="0 0 {canvas_width} {canvas_height}"
     role="img" aria-labelledby="title description">
  <title id="title">GNU/MOS — {title}</title>
  <desc id="description">Guest screenshot in a uniform light-gray frame with subtle edge highlights and layered shadows on a transparent background.</desc>
  <defs>
    <filter id="shadow" filterUnits="userSpaceOnUse" x="0" y="0"
            width="{canvas_width}" height="{canvas_height}" color-interpolation-filters="sRGB">
      <feGaussianBlur in="SourceAlpha" stdDeviation="{margin / 5:.1f}"/>
      <feOffset dx="{depth / 2:.1f}" dy="{depth * 1.5:.1f}"/>
      <feComponentTransfer result="ambient"><feFuncA type="linear" slope="0.32"/></feComponentTransfer>
      <feFlood flood-color="#0f172a"/>
      <feComposite in2="ambient" operator="in"/>
    </filter>
    <filter id="contact-shadow" filterUnits="userSpaceOnUse" x="0" y="0"
            width="{canvas_width}" height="{canvas_height}" color-interpolation-filters="sRGB">
      <feGaussianBlur in="SourceAlpha" stdDeviation="{rim / 2:.1f}"/>
      <feOffset dy="{depth / 2:.1f}"/>
      <feComponentTransfer><feFuncA type="linear" slope="0.25"/></feComponentTransfer>
    </filter>
    <clipPath id="screen">
      <rect x="{margin}" y="{margin}" width="{width}" height="{height}" rx="{rim}"/>
    </clipPath>
  </defs>
  <rect x="{frame_x}" y="{frame_y}" width="{frame_width}" height="{frame_height}"
        rx="{radius}" filter="url(#shadow)"/>
  <rect x="{frame_x}" y="{frame_y}" width="{frame_width}" height="{frame_height}"
        rx="{radius}" filter="url(#contact-shadow)"/>
  <rect x="{frame_x}" y="{frame_y}" width="{frame_width}" height="{frame_height}"
        rx="{radius}" fill="#cbd5e1" stroke="#ffffff" stroke-opacity="0.65"/>
  <image x="{margin}" y="{margin}" width="{width}" height="{height}"
         clip-path="url(#screen)" xlink:href="data:image/png;base64,{payload}"/>
  <rect x="{margin}" y="{margin}" width="{width}" height="{height}" rx="{rim}"
        fill="none" stroke="#94a3b8" stroke-opacity="0.55" stroke-width="{max(1, rim / 4):.1f}"/>
</svg>
'''


def main() -> None:
    output = SCREENSHOTS / "framed"
    output.mkdir(exist_ok=True)
    for path in sorted(SCREENSHOTS.glob("*.png")):
        (output / (path.stem + ".svg")).write_text(frame(path), encoding="utf-8")


if __name__ == "__main__":
    main()
