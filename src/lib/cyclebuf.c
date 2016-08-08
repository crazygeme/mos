#include <cyclebuf.h>
#include <lock.h>
#include <klib.h>
#include <config.h>
#include <vfs.h>
#include <ps.h>

typedef struct _cy_buf
{
    unsigned len;
    unsigned write_idx;
    unsigned read_idx;
    semaphore lock;
    spinlock idx_lock;
    int write_closed;
    unsigned char buf[PIPE_BUF_LEN];
}cy_buf;


cy_buf* cyb_create(char* name)
{
    cy_buf* ret = kmalloc(sizeof(*ret));
    int i = 0;

    for (i = 0; i < PIPE_BUF_LEN; i++)
    {
        ret->buf[i] = 0;
    }

    ret->len = ret->read_idx = ret->write_idx = 0;
    ret->write_closed = 0;
    sema_init(&ret->lock, name, 1);
    spinlock_init(&ret->idx_lock);

    return ret;
}

void cyb_putc(cy_buf* b, unsigned char key)
{
    unsigned length = 0;
    unsigned write_idx;
    int needs_trigger = 0;

    length = b->len;


    if (length == 0)
        needs_trigger = 1;

    spinlock_lock(&b->idx_lock);

    write_idx = b->write_idx;
    b->buf[write_idx] = key;
    write_idx++;
    if (write_idx == PIPE_BUF_LEN)
        write_idx = 0;
    b->write_idx = write_idx;

    if (b->write_idx == b->read_idx)
    {
        b->read_idx++;
        if (b->read_idx == PIPE_BUF_LEN)
        {
            b->read_idx = 0;
        }
        b->len--;
    }
    b->len++;

    spinlock_unlock(&b->idx_lock);

    if (key == EOF)
        b->write_closed = 1;

    if (needs_trigger)
        sema_trigger(&b->lock);

}

unsigned char cyb_getc(cy_buf* b)
{
    unsigned length = 0;
    length = b->len;
    unsigned read_idx;
    unsigned char ret;
    task_struct* cur = CURRENT_TASK();


    if (length == 0)
    {
        if (b->write_closed)
        {
            return EOF;
        }
        sema_wait(&b->lock);
    }

    spinlock_lock(&b->idx_lock);

    read_idx = b->read_idx;
    ret = b->buf[read_idx];
    read_idx++;
    if (read_idx == PIPE_BUF_LEN)
        read_idx = 0;

    b->read_idx = read_idx;
    b->len--;
    if (b->len == 0)
        sema_reset(&b->lock);

    spinlock_unlock(&b->idx_lock);

    return ret;

}

int cyb_isempty(cy_buf* b)
{
    return (b->len == 0);
}

int cyb_isfull(cy_buf* b)
{
    return (b->len == PIPE_BUF_LEN);
}

int cyb_is_writer_closed(cy_buf* b)
{
    return b->write_closed;
}

void cyb_writer_close(cy_buf* b)
{
    cyb_putc(b, EOF);

    b->write_closed = 1;
}
