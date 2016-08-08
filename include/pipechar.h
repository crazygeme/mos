#ifndef _PIPE_H_
#define _PIPE_H_

typedef struct _INODE* INODE;
typedef struct _cy_buf cy_buf;

void pipe_init();

INODE pipe_create_reader(cy_buf* buf);

INODE pipe_create_writer(cy_buf* buf);

#endif
