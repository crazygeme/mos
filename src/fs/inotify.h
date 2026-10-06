#ifndef MOS_FS_INOTIFY_H
#define MOS_FS_INOTIFY_H

#include <fs/fs.h>
#include <fs/vfs.h>

#define IN_ACCESS 0x00000001u
#define IN_MODIFY 0x00000002u
#define IN_ATTRIB 0x00000004u
#define IN_CLOSE_WRITE 0x00000008u
#define IN_CLOSE_NOWRITE 0x00000010u
#define IN_OPEN 0x00000020u
#define IN_MOVED_FROM 0x00000040u
#define IN_MOVED_TO 0x00000080u
#define IN_CREATE 0x00000100u
#define IN_DELETE 0x00000200u
#define IN_DELETE_SELF 0x00000400u
#define IN_MOVE_SELF 0x00000800u
#define IN_UNMOUNT 0x00002000u
#define IN_Q_OVERFLOW 0x00004000u
#define IN_IGNORED 0x00008000u
#define IN_ONLYDIR 0x01000000u
#define IN_DONT_FOLLOW 0x02000000u
#define IN_EXCL_UNLINK 0x04000000u
#define IN_MASK_CREATE 0x10000000u
#define IN_MASK_ADD 0x20000000u
#define IN_ISDIR 0x40000000u
#define IN_ONESHOT 0x80000000u
#define IN_ALL_EVENTS 0x00000fffu

struct inotify_event {
	int32_t wd;
	uint32_t mask, cookie, len;
};

typedef struct inotify_node inotify_node;
inotify_node *inotify_snapshot(super_block *root, const char *path);
void inotify_snapshot_put(inotify_node *node);
void inotify_created(super_block *root, const char *path);
void inotify_removed(inotify_node *node);
void inotify_linked(inotify_node *oldnode, super_block *root, const char *path);
void inotify_renamed(inotify_node *source, inotify_node *replacement,
		     super_block *root, const char *newpath);
void inotify_unmounted(super_block *sb);
void inotify_file_open(file *fp, super_block *root);
void inotify_file_event(file *fp, unsigned mask);
void inotify_file_close(file *fp);
void inotify_path_event(super_block *root, const char *path, unsigned mask);

/* Values are shared by procfs controls and quota enforcement. */
enum inotify_limit {
	INOTIFY_MAX_USER_WATCHES,
	INOTIFY_MAX_USER_INSTANCES,
	INOTIFY_MAX_QUEUED_EVENTS,
};
unsigned inotify_limit_get(unsigned which);
int inotify_limit_set(unsigned which, unsigned value);

#endif
