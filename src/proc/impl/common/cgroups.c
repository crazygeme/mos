#include "common.h"
#include <macro.h>
#include <ps/cgroup.h>
static void fill(proc_buf_t *pb)
{
	proc_buf_printf(
		pb,
		"#subsys_name\thierarchy\tnum_cgroups\tenabled\npids\t1\t%u\t1\ndevices\t2\t%u\t1\n",
		ps_cgroup_count(0), ps_cgroup_count(1));
}
DEFINE_PROC_FILE(cgroups, fill);
