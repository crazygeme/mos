#!/usr/bin/python

import os
import re
import sys

if not os.access("./out/assemble.s", os.R_OK):
    print("No assemble.s exist!")
    quit()


addr_to_name = {}
code_patten = '^[0-9a-f]+:.*$'
func_name_patten = '^[0-9a-f]+ <.*>:$'
gdb_dump_patten = '^#[0-9]+[\s]+0x[0-9a-f]+[\s]+in.*$'
dsf = open("./out/assemble.s")
symbols = dsf.readlines()
cur_func_name = ""
for i in range(0, len(symbols)):
    s = symbols[i]
    if re.match(func_name_patten, s):
        left = s.index('<')
        right = s.index('>')
        cur_func_name = s[left+1:right]
        continue
    if not re.match(code_patten, s):
        continue
    index = s.index(':')
    s = '0x'+ s[0:index]
    addr = int(s, 16)
    addr_to_name[addr] = cur_func_name

dsf.close()

def get_func_name(addr):
    if addr in addr_to_name.keys():
        return addr_to_name[addr]
    else:
        return "(user_mode_function)"

function_called_times = {}

f = open(dumpfile)
for line in f:
    if line.startswith("[profiling]0x"):
        line = line.replace("[profiling]", "")
        line = line.replace("\r", "").replace("\n", "").replace(" ", "")
    elif re.match(gdb_dump_patten, line):
        line = line.split()[1]
    else:
        continue
    address = int(line, 16)
    func_name = get_func_name(address)
    if func_name == "vfs_close":
        print("{0:x}: {1}".format(address, func_name))

    if func_name in function_called_times.keys():
        function_called_times[func_name] += 1
    else:
        function_called_times[func_name] = 1
    # print "{0:x} : {1}".format(address, get_func_name(address))

f.close()

sorted_map = sorted(function_called_times.items(), lambda x, y: cmp(y[1], x[1]))
user_mode_count = 0
kernel_mode_count = 0
for item in sorted_map:
    if item[0] == "(user_mode_function)":
        user_mode_count = item[1]
    else:
        kernel_mode_count += item[1]
    print("{0}: {1}".format(item[0], item[1]))

print("user   mode: total {0} ({1})".format(user_mode_count, float(user_mode_count) / (user_mode_count+kernel_mode_count)))
print("kernel mode: total {0} ({1})".format(kernel_mode_count, float(kernel_mode_count) / (user_mode_count+kernel_mode_count)))
