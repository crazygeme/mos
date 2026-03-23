dependence
-----------------------------------------------
i386-elf version of build tools, including gcc, ld, etc
qemu i386 enumlator
python for profiling
i386-elf version of gdb for debugging

instalation
------------------------------------------------
No other instalations required.

* ./rh9.sh bash			:for general purpos
* ./rh9.sh bash debug 		:for debugging
* ./rh9.sh tap bash		:with NAT networks
* ./rh9.sh			:experimental system V init procedure
* ./run.sh help			:print help info



profiling
-----------------------------------------------
1.	Run profiling.sh, it will wait until you start an os instance
2.	Run "./run.sh debug"
3.	Press any key in the shell running profiling.sh, then it will
	wait until you want to profiling
4.	Run some tasks in mos
5.	Press any key in the shell running profiling.sh, then it starts
	to profiling
6.	Press ctrl-c in the shell running profiling.sh, then it will 
	generate a profiling result

Version:
![Screen shot: versions](./screenshot/versions.png)

Vim:
![Screen shot: vim](./screenshot/vim.png)

List
![Screen shot: ls](./screenshot/ls.png)

/proc file system
![Screen shot: ls](./screenshot/proc.png)

Signals
![Screen shot: send](./screenshot/send_signal.png)
![Screen shot: recv](./screenshot/handle_signal.png)

Network
![Screen shot: network](./screenshot/network.png)
