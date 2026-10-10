# GNU/MOS Screenshots

The screenshots show the x64 GNU/MOS image launched by `./lfs run`, with two
KVM virtual CPUs, 8192 MiB RAM, a VirGL-capable VirtIO GPU, and an emulated
e1000 network adapter. The desktop uses Xorg, XDM, and Xfce. Desktop captures
use the guest resolution of 1920 × 1080 pixels.

Images use a consistent display width and retain their original proportions.
Click any image to view the full-resolution PNG.

[Desktop](#desktop) · [Boot and Login](#boot-and-login) ·
[System Information](#system-information) · [Editors](#editors) ·
[Browser](#browser) · [File System](#file-system) · [Signals](#signals) ·
[Networking](#networking) · [Processes](#processes) ·
[Capture and Gallery Assets](#capture-and-gallery-assets)

## Desktop

The Xfce session provides desktop panels, application launchers, and Thunar
file management. The desktop application view includes Mousepad and MATE
System Monitor.

### Xfce Desktop

<p align="center">
  <a href="screenshot/gui1.png">
    <img src="screenshot/framed/gui1.svg" alt="GNU/MOS Xfce desktop" width="800">
  </a>
</p>

### Desktop Applications

<p align="center">
  <a href="screenshot/gui2.png">
    <img src="screenshot/framed/gui2.svg" alt="GNU/MOS desktop applications" width="800">
  </a>
</p>

### AMD64 Memory View

The AMD64 memory view displays `uname -m` and `/proc/meminfo`. Managed memory
excludes regions reserved by the kernel and devices.

<p align="center">
  <a href="screenshot/x64_8g_desktop.png">
    <img src="screenshot/framed/x64_8g_desktop.svg" alt="AMD64 desktop with 8 GiB configured RAM" width="800">
  </a>
</p>

## Boot and Login

### Kernel and SysV Init Startup

<p align="center">
  <a href="screenshot/boot.png">
    <img src="screenshot/framed/boot.svg" alt="MOS startup" width="800">
  </a>
</p>

### XDM Login

<p align="center">
  <a href="screenshot/login_prompt.png">
    <img src="screenshot/framed/login_prompt.svg" alt="XDM login prompt" width="800">
  </a>
</p>

### Desktop Account

The authenticated terminal view displays the desktop account, its groups, and
the Xfce session environment.

<p align="center">
  <a href="screenshot/login_succ.png">
    <img src="screenshot/framed/login_succ.svg" alt="Authenticated desktop session" width="800">
  </a>
</p>

### Kernel Statistics

The kernel statistics view displays MOS memory allocation and page-fault
counters from `/proc/mos`.

<p align="center">
  <a href="screenshot/final.png">
    <img src="screenshot/framed/final.svg" alt="MOS kernel statistics" width="800">
  </a>
</p>

## System Information

### Kernel and CPU

MOS exposes Linux-compatible identity fields through `uname` and
`/proc/version`. The Linux release string identifies the compatibility
interface; the operating-system kernel is MOS.

<p align="center">
  <a href="screenshot/version.png">
    <img src="screenshot/framed/version.svg" alt="Kernel compatibility identity and CPU information" width="800">
  </a>
</p>

### GNU Userspace

The userspace version view displays the installed glibc, Bash, GCC, GNU Make,
Vim, Xorg, and Xfce versions.

<p align="center">
  <a href="screenshot/versions.png">
    <img src="screenshot/framed/versions.svg" alt="GNU userspace versions" width="800">
  </a>
</p>

## Editors

### Vim

Vim displays C source in a graphical terminal.

<p align="center">
  <a href="screenshot/vim.png">
    <img src="screenshot/framed/vim.svg" alt="Vim displaying C source" width="800">
  </a>
</p>

### Visual Studio Code

Visual Studio Code displays C source with syntax highlighting in the Xfce
session. The GNU/MOS launcher supplies `--no-sandbox` because MOS does not
implement Chromium namespace and seccomp isolation. Workspace Restricted Mode
is displayed by the editor.

<p align="center">
  <a href="screenshot/vscode.png">
    <img src="screenshot/framed/vscode.svg" alt="Visual Studio Code displaying C source" width="800">
  </a>
</p>

## Browser

### Microsoft Edge

Microsoft Edge displays the Baidu home page at `https://www.baidu.com`.
Navigation to `https://baidu.com` resolves to this address. The GNU/MOS
launcher supplies `--no-sandbox`; the browser displays the associated warning.

<p align="center">
  <a href="screenshot/browser.png">
    <img src="screenshot/framed/browser.svg" alt="Microsoft Edge displaying the Baidu home page" width="800">
  </a>
</p>

## File System

The directory view displays the root filesystem and the desktop account's
home directory. The procfs view lists process and system entries and displays
memory and uptime information.

### Directory Listing

<p align="center">
  <a href="screenshot/ls.png">
    <img src="screenshot/framed/ls.svg" alt="Root and home directory listings" width="800">
  </a>
</p>

### procfs

<p align="center">
  <a href="screenshot/proc.png">
    <img src="screenshot/framed/proc.svg" alt="MOS procfs entries" width="800">
  </a>
</p>

## Signals

The signal-delivery view uses shell job control to stop, continue, and
terminate a child process. The handler view uses a Bash `SIGUSR1` trap.

### Process Signal Delivery

<p align="center">
  <a href="screenshot/send_signal.png">
    <img src="screenshot/framed/send_signal.svg" alt="STOP, CONT, and TERM delivery" width="800">
  </a>
</p>

### Shell Signal Handler

<p align="center">
  <a href="screenshot/handle_signal.png">
    <img src="screenshot/framed/handle_signal.svg" alt="Bash SIGUSR1 handler" width="800">
  </a>
</p>

## Networking

### Network Configuration

The network configuration view displays the guest interfaces, DNS resolver
configuration, and routing table. The launcher provides DHCP, DNS, and IPv4
NAT through the host TAP interface.

<p align="center">
  <a href="screenshot/network.png">
    <img src="screenshot/framed/network.svg" alt="Guest network configuration" width="800">
  </a>
</p>

### ICMP Echo

<p align="center">
  <a href="screenshot/ping.png">
    <img src="screenshot/framed/ping.svg" alt="Ping to baidu.com" width="800">
  </a>
</p>

### DNS Resolution

<p align="center">
  <a href="screenshot/dns.png">
    <img src="screenshot/framed/dns.svg" alt="Baidu address resolution" width="800">
  </a>
</p>

### HTTPS Download

<p align="center">
  <a href="screenshot/wget.png">
    <img src="screenshot/framed/wget.svg" alt="GNU Wget HTTPS download" width="800">
  </a>
</p>

### SSH Login

<p align="center">
  <a href="screenshot/ssh_login.png">
    <img src="screenshot/framed/ssh_login.svg" alt="SSH session in the guest desktop" width="800">
  </a>
</p>

### Network Counters and ARP Cache

The network counters view displays interface traffic from `/proc/net/dev` and
the ARP cache from `/proc/net/arp`. These counters describe the active session;
they do not establish a maximum TCP throughput.

<p align="center">
  <a href="screenshot/throughput.png">
    <img src="screenshot/framed/throughput.svg" alt="Network transfer counters and ARP cache" width="800">
  </a>
</p>

## Processes

The process views list system services, processes owned by the desktop
account, and all processes.

### System Services

<p align="center">
  <a href="screenshot/ps_session0.png">
    <img src="screenshot/framed/ps_session0.svg" alt="System-account processes" width="800">
  </a>
</p>

### Desktop Account Processes

<p align="center">
  <a href="screenshot/ps_session1.png">
    <img src="screenshot/framed/ps_session1.svg" alt="Desktop-account processes" width="800">
  </a>
</p>

### All Processes

<p align="center">
  <a href="screenshot/ps_all.png">
    <img src="screenshot/framed/ps_all.svg" alt="All-process overview" width="800">
  </a>
</p>

### Process Activity

The `top` view displays live process activity and memory usage.

<p align="center">
  <a href="screenshot/top.png">
    <img src="screenshot/framed/top.svg" alt="Process activity in top" width="800">
  </a>
</p>
