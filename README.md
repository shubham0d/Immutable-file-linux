# Immutable-file-linux
A small fun project to make any file in linux system immutable/non writable by hooking `sys_openat` and `sys_write` system calls using ftrace.

## Instructions
* Change the target file path(default set to /tmp/test.txt) in `fh_sys_openat` funtion.
* Run `make` from terminal
* Load the module using `sudo insmod immutable_file.ko`

Reference for: https://nixhacker.com/hooking-syscalls-in-linux-using-ftrace
