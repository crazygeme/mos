#ifndef MOS_FS_EPOLL_H
#define MOS_FS_EPOLL_H
#include <fs/fs.h>
#include <ps/signal.h>

#define EPOLLIN 0x001U
#define EPOLLPRI 0x002U
#define EPOLLOUT 0x004U
#define EPOLLERR 0x008U
#define EPOLLHUP 0x010U
#define EPOLLRDNORM 0x040U
#define EPOLLRDBAND 0x080U
#define EPOLLWRNORM 0x100U
#define EPOLLWRBAND 0x200U
#define EPOLLRDHUP 0x2000U
#define EPOLLEXCLUSIVE (1U << 28)
#define EPOLLONESHOT (1U << 30)
#define EPOLLET (1U << 31)
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

/* Both i386 and AMD64 use a twelve-byte event record. */
struct epoll_event {
	uint32_t events;
	uint64_t data;
} __attribute__((packed));
struct epoll_timespec64 {
	int64_t tv_sec, tv_nsec;
};
int sys_epoll_create(int size);
int sys_epoll_create1(int flags);
int sys_epoll_ctl(int epfd, int op, int fd, const struct epoll_event *event);
int sys_epoll_wait(int epfd, struct epoll_event *events, int maxevents,
		   int timeout);
int sys_epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
		    int timeout, const sigset_t *mask, unsigned masksize);
int sys_epoll_pwait2(int epfd, struct epoll_event *events, int maxevents,
		     const struct epoll_timespec64 *timeout,
		     const sigset_t *mask, unsigned masksize);
void epoll_release_file(file *fp);
#endif
