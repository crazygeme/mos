#ifndef _SELECT_H
#define _SELECT_H
typedef int __fd_mask;
#define __FD_SETSIZE 1024
#define __NFDBITS	(8 * sizeof (__fd_mask))
#define	__FDELT(d)	((d) / __NFDBITS)
#define	__FDMASK(d)	((__fd_mask) 1 << ((d) % __NFDBITS))
typedef struct
  {

    __fd_mask __fds_bits[__FD_SETSIZE / __NFDBITS];
# define __FDS_BITS(set) ((set)->__fds_bits)

  } fd_set;

#define	FD_SETSIZE  __FD_SETSIZE

typedef __fd_mask fd_mask;

#define NFDBITS    __NFDBITS


#define __FD_ZERO_STOS "stosl"


#define __FD_ZERO(fdsp) memset(fdsp, 0, sizeof(fd_set))

#define __FD_SET(d, set) \
  ((void) (__FDS_BITS (set)[__FDELT (d)] |= __FDMASK (d)))

#define __FD_CLR(d, set) \
  ((void) (__FDS_BITS (set)[__FDELT (d)] &= ~__FDMASK (d)))

#define __FD_ISSET(d, set) \
  ((__FDS_BITS (set)[__FDELT (d)] & __FDMASK (d)) != 0)


#define	FD_SET(fd, fdsetp) __FD_SET (fd, fdsetp)
#define	FD_CLR(fd, fdsetp) __FD_CLR (fd, fdsetp)
#define	FD_ISSET(fd, fdsetp) __FD_ISSET (fd, fdsetp)
#define	FD_ZERO(fdsetp) __FD_ZERO (fdsetp)

int do_select(int nfds, fd_set *readfds, fd_set *writefds,
                   fd_set *exceptfds, const struct timespec *timeout,
                   void *sigmask);

#endif