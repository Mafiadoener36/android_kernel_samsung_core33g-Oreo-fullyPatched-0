/*
+ * Debug helper to dump the current kernel pagetables of the system
+ * so that we can see what the various memory ranges are set to.
+ *
+ * Derived from x86 implementation:
+ * (C) Copyright 2008 Intel Corporation
+ *
+ * Author: Arjan van de Ven <arjan@linux.intel.com>
+ *
+ * This program is free software; you can redistribute it and/or
+ * modify it under the terms of the GNU General Public License
+ * as published by the Free Software Foundation; version 2
+ * of the License.
+ */
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/seq_file.h>

#include <asm/fixmap.h>
#include <asm/pgtable.h>

static unsigned long cma_virt_start = 0;
static unsigned long cma_virt_end = 0;

unsigned long cache_type = 0, buffer_type = 0;
int temp_flag = 1;

struct addr_marker {
	unsigned long start_address;
	const char *name;
};

static struct addr_marker address_markers[] = {
	{ MODULES_VADDR,	"Modules" },
	{ PAGE_OFFSET,		"Kernel Mapping" },
	{ 0,			"vmalloc() Area" },
	{ VMALLOC_END,		"vmalloc() End" },
	{ FIXADDR_START,	"Fixmap Area" },
	{ CONFIG_VECTORS_BASE,	"Vectors" },
	{ CONFIG_VECTORS_BASE + PAGE_SIZE * 2, "Vectors End" },
	{ -1,			NULL },
};

struct pg_state {
	struct seq_file *seq;
	const struct addr_marker *marker;
	unsigned long start_address;
	unsigned level;
	u64 current_prot;
};

struct prot_bits {
	u64		mask;
	u64		val;
	const char	*set;
	const char	*clear;
};

static const struct prot_bits pte_bits[] = {
	{
		.mask	= L_PTE_USER,
		.val	= L_PTE_USER,
		.set	= "USR",
		.clear	= "   ",
	}, {
		.mask	= L_PTE_RDONLY,
		.val	= L_PTE_RDONLY,
		.set	= "ro",
		.clear	= "RW",
	}, {
		.mask	= L_PTE_XN,
		.val	= L_PTE_XN,
		.set	= "NX",
		.clear	= "x ",
	}, {
		.mask	= L_PTE_SHARED,
		.val	= L_PTE_SHARED,
		.set	= "SHD",
		.clear	= "   ",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_UNCACHED,
		.set	= "SO/UNCACHED",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_BUFFERABLE,
		.set	= "MEM/BUFFERABLE/WC",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_WRITETHROUGH,
		.set	= "MEM/CACHED/WT",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_WRITEBACK,
		.set	= "MEM/CACHED/WBRA",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_MINICACHE,
		.set	= "MEM/MINICACHE",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_WRITEALLOC,
		.set	= "MEM/CACHED/WBWA",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_DEV_SHARED,
		.set	= "DEV/SHARED",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_DEV_NONSHARED,
		.set	= "DEV/NONSHARED",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_DEV_WC,
		.set	= "DEV/WC",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_DEV_CACHED,
		.set	= "DEV/CACHED",
	},
};

static const struct prot_bits section_bits[] = {
	/* These are approximate */
	{
		.mask	= PMD_SECT_AP_READ | PMD_SECT_AP_WRITE,
		.val	= 0,
		.set	= "    ro",
	}, {
		.mask	= PMD_SECT_AP_READ | PMD_SECT_AP_WRITE,
		.val	= PMD_SECT_AP_WRITE,
		.set	= "    RW",
	}, {
		.mask	= PMD_SECT_AP_READ | PMD_SECT_AP_WRITE,
		.val	= PMD_SECT_AP_READ,
		.set	= "USR RO",
	}, {
		.mask	= PMD_SECT_AP_READ | PMD_SECT_AP_WRITE,
		.val	= PMD_SECT_AP_READ | PMD_SECT_AP_WRITE,
		.set	= "USR RW",
	}, {
		.mask	= PMD_SECT_XN,
		.val	= PMD_SECT_XN,
		.set	= "NX",
		.clear	= "x ",
	}, {
		.mask	= PMD_SECT_S,
		.val	= PMD_SECT_S,
		.set	= "SHD",
		.clear	= "   ",
	},
};

struct pg_level {
	const struct prot_bits *bits;
	size_t num;
	u64 mask;
};
static struct pg_level pg_level[] = {
	{
	}, { /* pgd */
	}, { /* pud */
	}, { /* pmd */
		.bits	= section_bits,
		.num	= ARRAY_SIZE(section_bits),
	}, { /* pte */
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	},
};

void cma_range_populate(unsigned long virt_start, unsigned long virt_end)
{
	cma_virt_start	= virt_start;
	cma_virt_end	= virt_end;
}

static void
dump_prot(struct pg_state *st,
		const struct prot_bits *bits, size_t num,unsigned long addr)
{
	unsigned i;

	for (i = 0; i < num; i++, bits++) {
		const char *s;

		if ((st->current_prot & bits->mask) == bits->val)
			s = bits->set;
		else
			s = bits->clear;

		if(s) {
			if(temp_flag != 0)
				seq_printf(st->seq, " %s", s);

			/* The cma_virt_start and cma_virt_end are the virtual
			 * addresses of the CMA(40mb) declared in defconfig file
			 */
			if( st->start_address >= cma_virt_start &&
				st->start_address < cma_virt_end &&
				addr > cma_virt_start && addr <= cma_virt_end ) {
				if(bits->val == L_PTE_MT_WRITETHROUGH ||
					bits->val == L_PTE_MT_WRITEBACK ||
					bits->val == L_PTE_MT_MINICACHE ||
					bits->val == L_PTE_MT_DEV_CACHED ||
					bits->val == L_PTE_MT_WRITEALLOC ) {
					unsigned long ctype =
						addr - st->start_address;
					cache_type += ctype;
				}
				if(bits->val == L_PTE_MT_BUFFERABLE ) {
					unsigned long btype =
						addr - st->start_address;
					buffer_type += btype;
				}
			}
		}
	}
}

static void
note_page(struct pg_state *st, unsigned long addr, unsigned level, u64 val)
{
	static const char units[] = "KMGTPE";
	u64 prot = val & pg_level[level].mask;

	if (addr < USER_PGTABLES_CEILING)
		return;

	if (!st->level) {
		st->level = level;
		st->current_prot = prot;
		if(temp_flag!=0)
			seq_printf(st->seq, "---[ %s ]---\n",
							st->marker->name);
	}
	else if (prot != st->current_prot || level != st->level ||
		   addr >= st->marker[1].start_address) {

		const char *unit = units;

		if (st->current_prot) {
			unsigned long delta;
			if(temp_flag!=0)
				seq_printf(st->seq, "0x%08lx-0x%08lx   ",
						st->start_address, addr);

			delta = (addr - st->start_address) >> 10;
			while (!(delta & 1023) && unit[1]) {
				delta >>= 10;
				unit++;
			}
			if(temp_flag!=0)
				seq_printf(st->seq, "%9lu%c", delta, *unit);

			if (pg_level[st->level].bits)
				dump_prot(st, pg_level[st->level].bits,
						pg_level[st->level].num, addr);

			if(temp_flag!=0)
				seq_printf(st->seq, "\n");
		}

		if (addr >= st->marker[1].start_address) {
			st->marker++;
			if(temp_flag!=0)
				seq_printf(st->seq,
					"---[ %s ]---\n", st->marker->name);
		}
		st->start_address = addr;
		st->current_prot = prot;
		st->level = level;
	}
}

static void walk_pte(struct pg_state *st, pmd_t *pmd, unsigned long start)
{
	pte_t *pte = pte_offset_kernel(pmd, 0);
	unsigned i;

	for (i = 0; i < PTRS_PER_PTE; i++, pte++) {
		unsigned long addr;
		addr = start + i * PAGE_SIZE;
		note_page(st, addr, 4, pte_val(*pte));
	}
}

static void walk_pmd(struct pg_state *st, pud_t *pud, unsigned long start)
{
	pmd_t *pmd = pmd_offset(pud, 0);
	unsigned i;

	for (i = 0; i < PTRS_PER_PMD; i++, pmd++) {
		unsigned long addr;
		addr = start + i * PMD_SIZE;
		if (pmd_none(*pmd) || pmd_large(*pmd) || !pmd_present(*pmd))
			note_page(st, addr, 3, pmd_val(*pmd));
		else
			walk_pte(st, pmd, addr);
	}
}

static void walk_pud(struct pg_state *st, pgd_t *pgd, unsigned long start)
{
	pud_t *pud = pud_offset(pgd, 0);
	unsigned i;

	for (i = 0; i < PTRS_PER_PUD; i++, pud++) {
		unsigned long addr;
		addr = start + i * PUD_SIZE;
		if (!pud_none(*pud)) {
			walk_pmd(st, pud, addr);
		} else {
			note_page(st, addr, 2, pud_val(*pud));
		}
	}
}
static void walk_pgd(struct seq_file *m)
{
	pgd_t *pgd = swapper_pg_dir;
	struct pg_state st;
	unsigned i;

	memset(&st, 0, sizeof(st));
	st.seq = m;
	st.marker = address_markers;

	cache_type = 0;
	buffer_type = 0;

	for (i = USER_PGTABLES_CEILING / PGDIR_SIZE;
	     i < PTRS_PER_PGD; i++, pgd++) {
		unsigned long addr;
		addr = i * PGDIR_SIZE;
		if (!pgd_none(*pgd)) {
			walk_pud(&st, pgd, addr);
		} else {
			note_page(&st, addr, 1, pgd_val(*pgd));
		}
	}

	note_page(&st, 0, 0, 0);
}

void cma_walk_pgd(struct seq_file *m)
{
	temp_flag = 0;
	walk_pgd(m);
	seq_printf(m,"Cacheable:				%lu kB\n",
							cache_type/SZ_1K);
	seq_printf(m,"Bufferable:				%lu kB\n",
							buffer_type/SZ_1K);
}

static int ptdump_show(struct seq_file *m, void *v)
{
	temp_flag = 1;
	walk_pgd(m);
	return 0;
}

static int ptdump_open(struct inode *inode, struct file *file)
{
	return single_open(file, ptdump_show, NULL);
}

static const struct file_operations ptdump_fops = {
	.open		= ptdump_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int ptdump_init(void)
{
	struct dentry *pe;
	unsigned i, j;

	for (i = 0; i < ARRAY_SIZE(pg_level); i++)
		if (pg_level[i].bits)
			for (j = 0; j < pg_level[i].num; j++)
				pg_level[i].mask |= pg_level[i].bits[j].mask;

	address_markers[2].start_address = VMALLOC_START;

	pe = debugfs_create_file("kernel_page_tables", 0400, NULL, NULL,
				 &ptdump_fops);
	return pe ? 0 : -ENOMEM;
}
__initcall(ptdump_init);
