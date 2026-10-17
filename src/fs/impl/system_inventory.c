/* Read-only CPU and physical-memory inventory. */
#include <fs/sysfs.h>
#include <ps/smp.h>
#include <mm/phymm.h>
#include <mm/mm.h>
#include <lib/klib.h>
#include <macro.h>
#include <lib/auto_free.h>

static void cpu_inventory(vfs_entry_node *root)
{
	vfs_entry_node *cpus =
		vfs_entry_directory_path(root, "devices/system/cpu", 0755);
	unsigned count = smp_cpu_count();
	char *text AUTO_FREE = malloc(128);
	char name[32];
	if (!text)
		return;
	sprintf(text, "0-%u\n", count - 1);
	vfs_entry_text(cpus, "possible", text);
	vfs_entry_text(cpus, "present", text);
	vfs_entry_text(cpus, "online", text);
	vfs_entry_text(cpus, "offline", "\n");
	sprintf(text, "%u\n", SMP_MAX_CPUS - 1);
	vfs_entry_text(cpus, "kernel_max", text);
	/* APIC IDs and CPUID topology describe the CPUs exposed by firmware. */
	unsigned a, b, c, d, max_leaf, threads = 1, package_shift = 0,
				       thread_shift = 0;
	arch_cpu_cpuid(0, 0, &max_leaf, &b, &c, &d);
	if (max_leaf >= 0xb) {
		for (unsigned sub = 0; sub < 8; sub++) {
			arch_cpu_cpuid(0xb, sub, &a, &b, &c, &d);
			if (!b)
				break;
			if (((c >> 8) & 0xff) == 1) {
				thread_shift = a & 31;
				threads = b;
			}
			if (((c >> 8) & 0xff) == 2)
				package_shift = a & 31;
		}
	}
	if (package_shift < thread_shift)
		package_shift = thread_shift;
	for (unsigned i = 0; i < count; i++) {
		sprintf(name, "cpu%u", i);
		vfs_entry_node *cpu = vfs_entry_directory(cpus, name);
		vfs_entry_text(cpu, "online", "1\n");
		vfs_entry_node *top = vfs_entry_directory(cpu, "topology");
		unsigned apic = smp_cpus[i].apic_id;
		unsigned package = package_shift ? apic >> package_shift : apic;
		unsigned core = thread_shift ? apic >> thread_shift : apic;
		if (package_shift)
			core &= (1U << (package_shift - thread_shift)) - 1;
		sprintf(text, "%u\n", package);
		vfs_entry_text(top, "physical_package_id", text);
		sprintf(text, "%u\n", core);
		vfs_entry_text(top, "core_id", text);
		vfs_entry_text(top, "die_id", "0\n");
		for (unsigned kind = 0; kind < 2; kind++) {
			unsigned length = 0, mask = 0;
			for (unsigned j = 0; j < count; j++) {
				unsigned other = smp_cpus[j].apic_id;
				int same =
					kind ? (package_shift ?
							(other >>
							 package_shift) ==
								package :
							j == i) :
					       (threads > 1 ?
							(other >>
							 thread_shift) ==
								(apic >>
								 thread_shift) :
							j == i);
				if (same) {
					mask |= 1U << j;
					length += sprintf(text + length, "%s%u",
							  length ? "," : "", j);
				}
			}
			strcpy(text + length, "\n");
			vfs_entry_text(top,
				       kind ? "core_siblings_list" :
					      "thread_siblings_list",
				       text);
			sprintf(text, "%08x\n", mask);
			vfs_entry_text(
				top, kind ? "core_siblings" : "thread_siblings",
				text);
		}
	}
}
static void memory_inventory(vfs_entry_node *root)
{
	vfs_entry_node *memory =
		vfs_entry_directory_path(root, "devices/system/memory", 0755);
	const unsigned pages = (128U * 1024 * 1024) / PAGE_SIZE;
	char name[32], text[32];
	vfs_entry_text(memory, "block_size_bytes", "8000000\n");
	for (unsigned first = 0; first < phymm_end; first += pages) {
		unsigned end = first + pages;
		if (end > phymm_end)
			end = phymm_end;
		unsigned p = first;
		while (p < end && phymm_pages[p].ref_count == PHYMM_RESERVED)
			p++;
		if (p == end)
			continue;
		sprintf(name, "memory%u", first / pages);
		vfs_entry_node *block = vfs_entry_directory(memory, name);
		sprintf(text, "%08x\n", first / pages);
		vfs_entry_text(block, "phys_index", text);
		vfs_entry_text(block, "state", "online\n");
		vfs_entry_text(block, "online", "1\n");
		vfs_entry_text(block, "removable", "0\n");
		vfs_entry_text(block, "valid_zones", "Normal\n");
	}
}
static void system_inventory(void)
{
	vfs_entry_node *root = vfs_entry_root(device_sysfs_entries());
	cpu_inventory(root);
	memory_inventory(root);
}
KERNEL_INIT(4, system_inventory);
