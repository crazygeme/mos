#ifndef _MOUNT_H_
#define _MOUNT_H_

struct filesys_type;

void mount_init();

int do_mount(char* path, struct filesys_type* type);

int do_umount(char* path);

struct filesys_type* mount_lookup(char* path);

#endif
