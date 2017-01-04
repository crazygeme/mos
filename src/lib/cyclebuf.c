#include <cyclebuf.h>
#include <lock.h>
#include <klib.h>
#include <config.h>
#include <ps.h>

typedef struct _cy_buf
{
    unsigned length;
    unsigned write_idx;
    unsigned read_idx;
    semaphore lock;
    spinlock idx_lock;
    int writer_count;
    int reader_count;
    unsigned ref_count;
    char *buf;
}cy_buf;

#define PIPE_REF_INCREASE(b)\
    __sync_add_and_fetch(&(b->ref_count), 1)

#define PIPE_REF_DECREASE(b)\
    __sync_add_and_fetch(&(b->ref_count), -1)

#define PIPE_LEN_ADD(b, len)\
    __sync_add_and_fetch(&(b->length), len)

#define PIPE_LEN(b)\
    __sync_add_and_fetch(&(b->length), 0)

#define PIPE_WRITER_INCREASE(b)\
    __sync_add_and_fetch(&(b->writer_count), 1)

#define PIPE_WRITER_DECREASE(b)\
    __sync_add_and_fetch(&(b->writer_count), -1)

#define PIPE_WRITERS(b)\
    __sync_add_and_fetch(&(b->writer_count), 0)

#define PIPE_READER_INCREASE(b)\
    __sync_add_and_fetch(&(b->reader_count), 1)

#define PIPE_READER_DECREASE(b)\
    __sync_add_and_fetch(&(b->reader_count), -1)

#define PIPE_READERS(b)\
    __sync_add_and_fetch(&(b->reader_count), 0)

cy_buf* cyb_create(char* name)
{
    cy_buf* ret = calloc(1, sizeof(*ret));
    int i = 0;
    ret->buf = vm_alloc(PIPE_BUF_LEN/PAGE_SIZE);

    ret->length = ret->read_idx = ret->write_idx = 0;
    ret->reader_count = ret->writer_count = 1;
    ret->ref_count = 2;
    sema_init(&ret->lock, name, 1);
    spinlock_init(&ret->idx_lock);

    return ret;
}

void cyb_destroy(cy_buf* b)
{
    if (PIPE_REF_DECREASE(b) == 0){
        vm_free(b->buf, PIPE_BUF_LEN/PAGE_SIZE);
        kfree(b);
    }
}

static void cyb_putc_internal(cy_buf* b, unsigned char key, int notifyOnWrite)
{
    unsigned length = 0;
    unsigned write_idx;
    int needs_trigger = 0;

    length = PIPE_LEN(b);

    if (notifyOnWrite)
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
        PIPE_LEN_ADD(b, -1);
    }
    PIPE_LEN_ADD(b, 1);

    spinlock_unlock(&b->idx_lock);

    if (notifyOnWrite)
        if (needs_trigger)
            sema_trigger(&b->lock);

}

void cyb_putc(cy_buf* b, unsigned char key)
{
    return cyb_putc_internal(b, key, 1);
}

void cyb_putbuf(cy_buf* b, unsigned char* buf, unsigned len)
{
    unsigned i = 0;
    int needs_trigger = 0;
    unsigned length = 0;

    length = PIPE_LEN(b);
    if (length == 0)
        needs_trigger = 1;

    for (i = 0; i < len; i++)
    {
        cyb_putc_internal(b, buf[i], 0);
    }

    if (needs_trigger)
        sema_trigger(&b->lock);
}

unsigned char cyb_getc(cy_buf* b)
{
    unsigned length = 0;

    unsigned read_idx;
    unsigned char ret;
    task_struct* cur = CURRENT_TASK();

    length = PIPE_LEN(b);
    if (length == 0){
        if (PIPE_WRITERS(b))
            sema_wait(&b->lock);
        if ((PIPE_WRITERS(b) == 0) &&
            (PIPE_LEN(b) == 0))
            return EOF;
    }

    spinlock_lock(&b->idx_lock);

    read_idx = b->read_idx;
    ret = b->buf[read_idx];
    read_idx++;
    if (read_idx == PIPE_BUF_LEN)
        read_idx = 0;

    b->read_idx = read_idx;
    PIPE_LEN_ADD(b, -1);
    if (PIPE_LEN(b) == 0)
        sema_reset(&b->lock);

    spinlock_unlock(&b->idx_lock);

    return ret;

}

int cyb_isempty(cy_buf* b)
{
    return (PIPE_LEN(b) == 0);
}

int cyb_isfull(cy_buf* b)
{
    return (PIPE_LEN(b) == PIPE_BUF_LEN);
}

int cyb_writer_count(cy_buf* b)
{
    return PIPE_WRITERS(b);
}

int cyb_reader_count(cy_buf* b)
{
    return PIPE_READERS(b);
}

int cyb_writer_close(cy_buf* b)
{
    PIPE_WRITER_DECREASE(b);
    sema_trigger(&b->lock);
    cyb_destroy(b);
}

int cyb_reader_close(cy_buf* b)
{
    PIPE_READER_DECREASE(b);
    cyb_destroy(b);
}

int cyb_get_buf_len(cy_buf* b)
{
    return PIPE_LEN(b);
}