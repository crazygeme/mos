# GNU/MOS Screenshots

The screenshots show the x64 GNU/MOS image launched by `./lfs run`, with two
KVM virtual CPUs, 8192 MiB RAM, a VirGL-capable VirtIO GPU, and an emulated
e1000 network adapter. The desktop uses Xorg, XDM, and Xfce. Desktop captures
use the guest resolution of 1920 × 1080 pixels.

The launcher supports capture without opening a host window:

```sh
MOS_AUDIO_BACKEND=none ./lfs run -- \
  -display egl-headless \
  -vnc 127.0.0.1:9
```

The VNC endpoint is `127.0.0.1:5909`. The EGL backend requires a compatible
host render node. VNC captures include the guest desktop and application
windows; boot captures show the text framebuffer. Gallery images link to
the original PNG captures. The SVG gallery images apply a soft drop
shadow on a transparent background. `python3 tools/frame_screenshots.py`
regenerates the SVG gallery images from `doc/screenshot/*.png`.

## Desktop

The Xfce session provides desktop panels, application launchers, and Thunar
file management. The desktop application view includes Mousepad and MATE
System Monitor.

[![GNU/MOS Xfce desktop](screenshot/framed/gui1.svg)](screenshot/gui1.png)

[![GNU/MOS desktop applications](screenshot/framed/gui2.svg)](screenshot/gui2.png)

The AMD64 memory view displays `uname -m` and `/proc/meminfo`. Managed memory
excludes regions reserved by the kernel and devices.

[![AMD64 desktop with 8 GiB configured RAM](screenshot/framed/x64_8g_desktop.svg)](screenshot/x64_8g_desktop.png)

## Boot and Login

| Kernel and SysV init startup | XDM login |
| --- | --- |
| [![MOS startup](screenshot/framed/boot.svg)](screenshot/boot.png) | [![XDM login prompt](screenshot/framed/login_prompt.svg)](screenshot/login_prompt.png) |

The authenticated terminal view displays the desktop account, its groups, and
the Xfce session environment. The kernel statistics view displays MOS memory
allocation and page-fault counters from `/proc/mos`.

| Desktop account | Kernel statistics |
| --- | --- |
| [![Authenticated desktop session](screenshot/framed/login_succ.svg)](screenshot/login_succ.png) | [![MOS kernel statistics](screenshot/framed/final.svg)](screenshot/final.png) |

## System Information

MOS exposes Linux-compatible identity fields through `uname` and
`/proc/version`. The Linux release string identifies the compatibility
interface; the operating-system kernel is MOS.

[![Kernel compatibility identity and CPU information](screenshot/framed/version.svg)](screenshot/version.png)

The userspace version view displays the installed glibc, Bash, GCC, GNU Make,
Vim, Xorg, and Xfce versions.

[![GNU userspace versions](screenshot/framed/versions.svg)](screenshot/versions.png)

## Editors

Vim displays C source in a graphical terminal.

[![Vim displaying C source](screenshot/framed/vim.svg)](screenshot/vim.png)

Visual Studio Code displays C source with syntax highlighting in the Xfce
session. The GNU/MOS launcher supplies `--no-sandbox` because MOS does not
implement Chromium namespace and seccomp isolation. Workspace Restricted Mode
is displayed by the editor.

[![Visual Studio Code displaying C source](screenshot/framed/vscode.svg)](screenshot/vscode.png)

## Browser

Microsoft Edge displays the Baidu home page at `https://www.baidu.com`.
Navigation to `https://baidu.com` resolves to this address. The GNU/MOS
launcher supplies `--no-sandbox`; the browser displays the associated warning.

[![Microsoft Edge displaying the Baidu home page](screenshot/framed/browser.svg)](screenshot/browser.png)

## File System

The directory view displays the root filesystem and the desktop account's
home directory. The procfs view lists process and system entries and displays
memory and uptime information.

| Directory listing | procfs |
| --- | --- |
| [![Root and home directory listings](screenshot/framed/ls.svg)](screenshot/ls.png) | [![MOS procfs entries](screenshot/framed/proc.svg)](screenshot/proc.png) |

## Signals

The signal-delivery view uses shell job control to stop, continue, and
terminate a child process. The handler view uses a Bash `SIGUSR1` trap.

| Process signal delivery | Shell signal handler |
| --- | --- |
| [![STOP, CONT, and TERM delivery](screenshot/framed/send_signal.svg)](screenshot/send_signal.png) | [![Bash SIGUSR1 handler](screenshot/framed/handle_signal.svg)](screenshot/handle_signal.png) |

## Networking

The network configuration view displays the guest interfaces, DNS resolver
configuration, and routing table. The launcher provides DHCP, DNS, and IPv4
NAT through the host TAP interface.

[![Guest network configuration](screenshot/framed/network.svg)](screenshot/network.png)

| ICMP echo | DNS resolution |
| --- | --- |
| [![Ping to baidu.com](screenshot/framed/ping.svg)](screenshot/ping.png) | [![Baidu address resolution](screenshot/framed/dns.svg)](screenshot/dns.png) |
| HTTPS download | SSH login |
| [![GNU Wget HTTPS download](screenshot/framed/wget.svg)](screenshot/wget.png) | [![SSH session in the guest desktop](screenshot/framed/ssh_login.svg)](screenshot/ssh_login.png) |

The network counters view displays interface traffic from `/proc/net/dev` and
the ARP cache from `/proc/net/arp`. These counters describe the active session;
they do not establish a maximum TCP throughput.

[![Network transfer counters and ARP cache](screenshot/framed/throughput.svg)](screenshot/throughput.png)

## Processes

The process views list system services, processes owned by the desktop
account, and all processes.
| System services | Desktop account |
| --- | --- |
| [![System-account processes](screenshot/framed/ps_session0.svg)](screenshot/ps_session0.png) | [![Desktop-account processes](screenshot/framed/ps_session1.svg)](screenshot/ps_session1.png) |

[![All-process overview](screenshot/framed/ps_all.svg)](screenshot/ps_all.png)

The `top` view displays live process activity and memory usage.

[![Process activity in top](screenshot/framed/top.svg)](screenshot/top.png)
