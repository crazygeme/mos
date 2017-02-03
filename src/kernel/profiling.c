//
// Created by ZhengEnder on 2016/12/7.
//

#include <profiling.h>
#include <rbtree.h>

static int started = 0;

void profiling_start()
{
    klog_printf("[profiling]start\n");
    started = 1;
}

void profiling_stop()
{
    started = 0;
    klog_printf("[profiling]stop\n");
}

void profiling_add_record(unsigned eip)
{
    if (started)
        klog_printf("[profiling]%x\n", eip);
}
