# Screenshots

## GUI Desktop

![MOS GUI desktop](screenshot/gui.png)

MOS now boots far enough to reach a visible graphical desktop under the Red Hat 9 userspace stack.

---

## Boot Sequence

|                                             |                                              |
| ------------------------------------------- | -------------------------------------------- |
| **Booting**                                 | **Login Prompt**                             |
| ![Booting](screenshot/boot.png)             | ![Login prompt](screenshot/login_prompt.png) |
| **Login Success**                           | **All Processes**                            |
| ![Login success](screenshot/login_succ.png) | ![All processes](screenshot/final.png)       |

---

## System Info

**Version**

![Versions](screenshot/versions.png)

---

## Editors

| Vim                        | Emacs                          |
| -------------------------- | ------------------------------ |
| ![Vim](screenshot/vim.png) | ![Emacs](screenshot/emacs.png) |

---

## File System

| `ls` command             | `/proc` file system          |
| ------------------------ | ---------------------------- |
| ![ls](screenshot/ls.png) | ![proc](screenshot/proc.png) |

---

## Signals

| Send signal                                | Receive signal                                 |
| ------------------------------------------ | ---------------------------------------------- |
| ![Send signal](screenshot/send_signal.png) | ![Handle signal](screenshot/handle_signal.png) |

---

## Networking

**Network stack**

![Network](screenshot/network.png)

| `ping`                       | DNS                              |
| ---------------------------- | -------------------------------- |
| ![ping](screenshot/ping.png) | ![dns](screenshot/dns.png)       |
| `wget`                       | `ssh`                            |
| ![wget](screenshot/wget.png) | ![ssh](screenshot/ssh_login.png) |

**TCP Throughput**

32K TCP_WND has a maximum of ~210 KB/s, confirming the OS is not the bottleneck.

| Window Size                 | Max Throughput |
| --------------------------- | -------------- |
| 2 KB                        | ~13 KB/s       |
| 32 KB                       | ~213 KB/s      |
| 212 KB (Linux default rmem) | ~1.4 MB/s      |
| 6 MB (Linux auto-tuned)     | ~40 MB/s       |

![Throughput](screenshot/throughput.png)

---

## Process Management

| `ps` session 0                              | `ps` session 1                              | `ps` all                         |
| ------------------------------------------- | ------------------------------------------- | -------------------------------- |
| ![ps session 0](screenshot/ps_session0.png) | ![ps session 1](screenshot/ps_session1.png) | ![ps all](screenshot/ps_all.png) |

**`top` command**

![top](screenshot/top.png)
