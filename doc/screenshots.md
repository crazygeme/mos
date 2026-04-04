# Screenshots

Booting
---
![Screen shot: login](screenshot/boot.png)

Login prompt
---
![Screen shot: login](screenshot/login_prompt.png)

Login success
---
![Screen shot: login](screenshot/login_succ.png)

All processes
---
![Screen shot: login](screenshot/final.png)


Version:
---
![Screen shot: versions](screenshot/versions.png)

Vim:
---
![Screen shot: vim](screenshot/vim.png)

Emacs:
---
![Screen shot: vim](screenshot/emacs.png)

List
---
![Screen shot: ls](screenshot/ls.png)

/proc file system
---
![Screen shot: proc](screenshot/proc.png)

Signals
---
<table>
  <tr>
    <td>Send signal</td>
    <td>Receive signal</td>
  <tr>
    <td><img src="screenshot/send_signal.png"></td>
    <td><img src="screenshot/handle_signal.png"></td>
  </tr>
</table>

Network
---
![Screen shot: network](screenshot/network.png)

`ping` actually works
---
![Screen shot: ping](screenshot/ping.png)

DNS actually works
---
![Screen shot: dns](screenshot/dns.png)

`wget` actually works
---
![Screen shot: dns](screenshot/wget.png)

Throughput ()
---
32K TCP_WND size has maximum 210KB/s, so the os is not bottleneck.

| Window                      | Maxthroughput |
| --------------------------- | ------------- |
| 2 KB                        | ~13 KB/s      |
| 32 KB                       | ~213 KB/s     |
| 212 KB (Linux default rmem) | ~1.4 MB/s     |
| 6 MB (Linux auto-tuned)     | ~40 MB/s      |

---
![Screen shot: throughput](screenshot/throughput.png)

`ps` command for session 0
---
![Screen shot: throughput](screenshot/ps_session0.png)

`ps` command for session 1
---
![Screen shot: throughput](screenshot/ps_session1.png)

`ps` command for all processes
---
![Screen shot: throughput](screenshot/ps_all.png)

`top` command actually works
---
![Screen shot: throughput](screenshot/top.png)
