// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/memory.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/memblock.h>
#include <linux/mmu_context.h>
#include <linux/mmzone.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox/qmp.h>
#include <linux/migrate.h>
#include <linux/swap.h>
#include <linux/mm_inline.h>
#include <linux/compaction.h>

static unsigned long start_section_nr, end_section_nr;
static unsigned int sections_per_block;

/*
 * adding a module parameter to get user define dram end address,
 * this address shall be considered to online the offlined memory
 */
static unsigned long long ram_size = 0;
MODULE_PARM_DESC(ram_size, "To avail user programmed DRAM end address");
module_param(ram_size, ullong, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

static phys_addr_t bootmem_dram_end_addr;

static phys_addr_t onlinable_region_start_addr;

static int mem_online_remaining_blocks(void)
{
	unsigned long memblock_end_pfn = __phys_to_pfn(memblock_end_of_DRAM());
	unsigned long ram_end_pfn = __phys_to_pfn(bootmem_dram_end_addr - 1);
	unsigned long block_size, memblock, pfn;
	unsigned int nid, delta;
	phys_addr_t phys_addr;
	int fail = 0;

	pr_debug("mem_online: memblock_end_of_DRAM 0x%lx\n", memblock_end_of_DRAM());

	block_size = memory_block_size_bytes();
	sections_per_block = block_size / MIN_MEMORY_BLOCK_SIZE;

	start_section_nr = pfn_to_section_nr(memblock_end_pfn);
	end_section_nr = pfn_to_section_nr(ram_end_pfn);

	/*
	 * since the Kernel CMDLINE is providing the memblock_end_of_DRAM
	 * added check to see if its within bootmem_dram_end_addr of DDR
	 */
	if (memblock_end_of_DRAM() >= bootmem_dram_end_addr) {
		pr_err("mem_online: System booted with no zone movable memory blocks. Cannot perform memory onlining\n");
		return -EINVAL;
	}

	if (memblock_end_of_DRAM() % block_size) {
		delta = block_size - (memblock_end_of_DRAM() % block_size);
		pr_debug("mem_online: memblock end of dram address not aligned to memory block size of %lukB!\n", block_size);
		pr_debug("mem_online: memory%lu is partially available; %lukB of memory will be less in this block\n", start_section_nr, delta / SZ_1K);

		/*
		 * since this section is partially added during boot, we cannot
		 * add the remaining part of section using add_memory since it
		 * won't be size aligned to block size. We have to start the
		 * onlinable region from the next section onwards.
		 */
		start_section_nr += 1;

	}

	if (bootmem_dram_end_addr % block_size) {
		delta = bootmem_dram_end_addr % block_size;
		pr_debug("mem_online: bootmem end of dram address is not aligned to memory block size!\n");
		pr_debug("mem_online: memory%lu will not be added; %lukB of memory will be less to match with block size!\n", end_section_nr, delta / SZ_1K);

		/*
		 * since this section cannot be added, the last section of onlinable
		 * region will be the previous section.
		 */
		end_section_nr -= 1;
	}

	onlinable_region_start_addr = section_nr_to_pfn(__pfn_to_phys(start_section_nr));

	/*
	 * below check holds true if there were only one onlinable section
	 * and that was partially added during boot. In such case, bail out.
	 */
	if (start_section_nr > end_section_nr)
		return 1;

	pr_debug("mem_online: onlinable_region_start_addr 0x%lx\n", onlinable_region_start_addr);
	pr_debug("mem_online: start_section_nr = %llu end_section_nr = %llu\n", start_section_nr, end_section_nr);

	for (memblock = start_section_nr; memblock <= end_section_nr;
			memblock += sections_per_block) {

		pfn = section_nr_to_pfn(memblock);
		phys_addr = __pfn_to_phys(pfn);

		if (phys_addr & (((PAGES_PER_SECTION * sections_per_block)
					<< PAGE_SHIFT) - 1)) {
			fail = 1;
			pr_warn("mem_online: PFN of mem%lu block not aligned to section start. Not adding this memory block\n",
								memblock);
			continue;
		}
		nid = memory_add_physaddr_to_nid(phys_addr);
		if (add_memory(nid, phys_addr,
				 MIN_MEMORY_BLOCK_SIZE * sections_per_block, MHP_NONE)) {
			pr_warn("mem_online: Adding memory block mem%lu failed\n", memblock);
			fail = 1;
		}
	}

	return fail;
}



static int update_dram_end_address(phys_addr_t *bootmem_dram_end_addr)
{
	struct device_node *node;
	struct property *prop;
	int len, num_cells, num_entries;
	u64 addr = 0, max_base = 0;
	u64 size, base, section_size;
	u64 movable_start;
	int nr_address_cells, nr_size_cells;
	const __be32 *pos;

	node = of_find_node_by_name(of_root, "memory");
	if (!node) {
		pr_err("mem_online: memory node not found in DT\n");
		return -EINVAL;
	}

	nr_address_cells = of_n_addr_cells(of_root);
	nr_size_cells = of_n_size_cells(of_root);

	prop = of_find_property(node, "reg", &len);
	if (!prop) {
		pr_err("mem_online: reg node not found in DT\n");
		return -EINVAL;
	}

	num_cells = len / sizeof(__be32);
	num_entries = num_cells / (nr_address_cells + nr_size_cells);

	pos = prop->value;

	section_size = MIN_MEMORY_BLOCK_SIZE;
	movable_start = memblock_end_of_DRAM();

	while (num_entries--) {
		base = of_read_number(pos, nr_address_cells);
		size = of_read_number(pos + nr_address_cells, nr_size_cells);
		pos += nr_address_cells + nr_size_cells;

		if (base > max_base) {
			max_base = base;
			addr = base + size;
		}
	}

	/*
	 * this is to check if the user defined DRAM end address is within
	 * the boundaries of actual DRAM end address of the device i.e., addr
	 * and greater than onlinable region start address movable_start here.
	 */
	if(addr > ram_size && movable_start < ram_size && ram_size > 0)
		addr = ram_size;

	/*
	 * enabling memory onlining until the user defined end addr if its valid;
	 * else onlining memory until the actual DRAM end address of device i.e., addr
	 */
	*bootmem_dram_end_addr = addr;
	pr_err("mem_online: *bootmem_dram_end_addr 0x%lx\n", *bootmem_dram_end_addr);

	return 0;

}

static int mem_online_driver_probe(struct platform_device *pdev)
{

	int ret;

	ret = update_dram_end_address(&bootmem_dram_end_addr);
	if (ret)
		return ret;

	ret = mem_online_remaining_blocks();

	return ret;
}

static struct platform_device mem_online_device = {
	.name = "mem_online",
	.id = -1,
};

static struct platform_driver mem_online_driver = {
	.probe = mem_online_driver_probe,
	.driver = {
		.name = "mem_online",
	},
};

static int __init mem_online_init(void)
{
	int ret = 0;

	ret = platform_device_register(&mem_online_device);
	if (ret)
		return -ENODEV;

	ret = platform_driver_register(&mem_online_driver);
	if (ret) {
		pr_err("%s: Failed to register qcom_subsys_status_driver\n", __func__);
	}

	return ret;
}

static void __exit mem_online_exit(void)
{
	platform_driver_unregister(&mem_online_driver);
}

module_init(mem_online_init);
module_exit(mem_online_exit);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. Memory Hotplug Driver");
MODULE_LICENSE("GPL");
