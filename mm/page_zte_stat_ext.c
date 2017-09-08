/*
 *  linux/mm/page_alloc.c
 *
 *  Manages the free list, the system allocates free pages here.
 *  Note that kmalloc() lives in slab.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 *  Reshaped it to be a zoned allocator, Ingo Molnar, Red Hat, 1999
 *  Discontiguous memory support, Kanoj Sarcar, SGI, Nov 1999
 *  Zone balancing, Kanoj Sarcar, SGI, Jan 2000
 *  Per cpu hot/cold page lists, bulk allocation, Martin J. Bligh, Sept 2002
 *          (lots of bits borrowed from Ingo Molnar & Andrew Morton)
 */

#include <linux/stddef.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/interrupt.h>
#include <linux/pagemap.h>
#include <linux/jiffies.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/kmemcheck.h>
#include <linux/kasan.h>
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/pagevec.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/ratelimit.h>
#include <linux/oom.h>
#include <linux/notifier.h>
#include <linux/topology.h>
#include <linux/sysctl.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/memory_hotplug.h>
#include <linux/nodemask.h>
#include <linux/vmalloc.h>
#include <linux/vmstat.h>
#include <linux/mempolicy.h>
#include <linux/stop_machine.h>
#include <linux/sort.h>
#include <linux/pfn.h>
#include <linux/backing-dev.h>
#include <linux/fault-inject.h>
#include <linux/page-isolation.h>
#include <linux/page_cgroup.h>
#include <linux/debugobjects.h>
#include <linux/kmemleak.h>
#include <linux/compaction.h>
#include <trace/events/kmem.h>
#include <linux/prefetch.h>
#include <linux/mm_inline.h>
#include <linux/migrate.h>
#include <linux/page-debug-flags.h>
#include <linux/hugetlb.h>
#include <linux/sched/rt.h>

#include "internal.h"
#include "page_zte_stat_ext.h"

/* ------------------------------------------- start of state interface */
enum page_zte_stats_state {
	STATS_UNINITIALIZED = 0, /* init state, not work */
	STATS_INITIALIZED = 1, /* init and can work */
	STATS_DISABLED = 2, /* user disable this optimization measure */
	STATS_ERROR = 3, /* got unexpected error, stop working */
};

/* each bit for one kind of error, 0 for no error, can be used as int */
enum page_zte_stats_error_flag {
	STATS_ERROR_NO_ERROR = 0,
	STATS_ERROR_ZONEIDX_INVALID = 1,
	STATS_ERROR_PAGES_UNEXPECTED = 2,
	STATS_ERROR_MIGRATETYPE_UNEXPECTED = 4,
	STATS_ERROR_OTHERS = 8,
};

/* pzs is the abbreviation of page zte stats */
/* we don't get mutex protection for state and flags. */
/* So be careful of weather it could be serious issue */
static int inner_pzs_state = STATS_UNINITIALIZED;
static unsigned int inner_pzs_error_flags;

/**
 * help for var get/set functions.
 * only set when state changed under no error status.
 */
inline int inner_pzs_get_state(void)
{
	return inner_pzs_state;
}
inline int inner_pzs_get_errorflag(void)
{
	return inner_pzs_error_flags;
}
/**
 * set when state changed if no error happened.
 * or set to error no matter what state to set.
 * @return: the state used after.
 */
inline int inner_pzs_set_state(enum page_zte_stats_state state)
{
	/* for stats, error has equal effection as disable */
	/* here STATS_ERROR just takes precedence */
	if (inner_pzs_error_flags &&
		inner_pzs_state != STATS_ERROR)
		inner_pzs_state = STATS_ERROR;
	else
		if (inner_pzs_state != state)
			inner_pzs_state = state;

	return inner_pzs_state;
}
/* not allowed to clear any flag set before */
inline int inner_pzs_set_errorflag(enum page_zte_stats_error_flag flag)
{
	inner_pzs_error_flags |= flag;
	if (inner_pzs_error_flags) /* possible reset the error state below */
		inner_pzs_set_state(STATS_ERROR);

	return inner_pzs_error_flags;
}
/* used when init */
inline void inner_pzs_reset_state(void)
{
	inner_pzs_error_flags = 0; /* reset */
	inner_pzs_state = STATS_INITIALIZED;
}
/* as judgement if the module is active or not */
int zte_page_stats_in_working(void)
{
	return inner_pzs_state ==  STATS_INITIALIZED;
}
/* ------------------------------------------- end of state interface */

/* ------------------------------------ start of param control interface */
enum {
	PZS_DISABLE = 0x0, /* disable page zte stats */
	PZS_ENABLE = 0x1, /* enable by default */
};
/* var for keeping the user enable value */
static int pzs_enable = PZS_ENABLE;
static int pzs_enable_set(const char *val, struct kernel_param *kp);
module_param_call(stats_enable, pzs_enable_set, param_get_int,
			&pzs_enable, 0644);
/**
 * Now only allowed from ENABLE to DISABLE.
 * @return: -EPERM for not allowed, 0 for normal.
 */
static int pzs_enable_set(const char *val, struct kernel_param *kp)
{
	int ret;
	int old_val = pzs_enable;

	/* limit from enable to disable */
	if (pzs_enable != PZS_ENABLE ||
		!zte_page_stats_in_working())
		return -EPERM;

	ret = param_set_int(val, kp);
	if (ret) /*unexpected param */
		return ret;

	/* limit from enable to disable */
	if (pzs_enable != PZS_DISABLE) {
		pzs_enable = old_val;
		return -EINVAL;
	}

	/* should be disable now */
	inner_pzs_set_state(STATS_DISABLED);
	pr_info("pzs enable off from on");

	return 0;
}
/* ------------------------------------ end of param control interface */

/**
 * a general purpose var not supporting parallel using
 * to provide a stat to accumulated a var.
 */
static long inner_page_zte_global_long_var;
void zte_page_stats_gl_reset(void)
{
	inner_page_zte_global_long_var = 0;
}
void zte_page_stats_gl_add(long delta)
{
	inner_page_zte_global_long_var += delta;
}
long zte_page_stats_gl_get(void)
{
	return inner_page_zte_global_long_var;
}

static long inner_page_zte_order_array[MAX_ORDER];
void inner_page_zte_stats_gla_reset(void)
{
	memset(inner_page_zte_order_array,
		0,
		sizeof(inner_page_zte_order_array));
}
void inner_page_zte_stats_gla_add(long delta, int order)
{
	inner_page_zte_order_array[order] += delta;
}
void inner_page_zte_stats_gla_print(struct seq_file *s)
{
	int order;

	for (order = 0; order < MAX_ORDER; ++order) {
		if (s != NULL)
			seq_printf(s, "%6ld ",
				inner_page_zte_order_array[order]);
		else
			pr_info("%6ld ",
				inner_page_zte_order_array[order]);
	}
}

struct page_zte_stat_item_type {
	atomic_long_t pages;
	/* how many pages are allocated in each migratetype */
	/* may minus if the alloc is not consistent with the free*/
	atomic_long_t demand[MAX_NR_ZONES][MIGRATE_TYPES][MAX_ORDER];
};
struct page_zte_stats_type {
	struct page_zte_stat_item_type cur;
	struct page_zte_stat_item_type max;
	struct page_zte_stat_item_type min;
	/* how many pages had moved between */
	long mov[MAX_NR_ZONES][MIGRATE_TYPES][MIGRATE_TYPES];
};

struct page_zte_stats_type page_zte_stats;

/* below names must be consistent with the fact used */
static char * const local_z_names[MAX_NR_ZONES] = {
#ifdef CONFIG_ZONE_DMA
	"DMA",
#endif
#ifdef CONFIG_ZONE_DMA32
	"DMA32",
#endif
	"NORMAL",
#ifdef CONFIG_HIGHMEM
	"HIGHMEM",
#endif
	"MOVABLE",
};
static char * const local_m_names[MIGRATE_TYPES] = {
	"Unmovable",
	"Reclaimable",
	"Movable",
#ifdef CONFIG_CMA
	"CMA",
#endif
	"Reserve",
#ifdef CONFIG_MEMORY_ISOLATION
	"Isolate",
#endif
};

/**
 * update the stats for migratetype move.
 * This is the zone based update and because is calling under
 * the protection of &zone->lock, so no need to atomic update.
 * @param zone: which zone the pages belong to.
 * @param pages_count: how many pages to mov.
 * @param migratetype: the target migratetype.
 * @param old_mt: the old migratetype before moving.
 * @return: value of page_zte_stats_error_flag int.
 */
int zte_page_stats_move_freepages(struct zone *zone,
			unsigned long pages_count,
			int migratetype, int old_mt)
{
	int ret = STATS_ERROR_NO_ERROR;
	int zone_idx = zone_idx(zone);

	if (migratetype >= MIGRATE_TYPES ||
		migratetype < MIGRATE_UNMOVABLE ||
		old_mt >= MIGRATE_TYPES ||
		old_mt < MIGRATE_UNMOVABLE ||
		pages_count != pageblock_nr_pages ||
		zone_idx >= MAX_NR_ZONES ||
		zone_idx < 0) /* valid zone_idx is 0-2 */
		ret = inner_pzs_set_errorflag(
			STATS_ERROR_MIGRATETYPE_UNEXPECTED);
	else
		page_zte_stats.mov[zone_idx][old_mt][migratetype] +=
			pages_count;

	return ret;
}

void inner_zte_page_stat_newline_print(struct seq_file *s)
{
	if (s != NULL)
		seq_putc(s, '\n');
	else
		pr_info("\n");
}

void inner_zte_prerror_help(struct seq_file *s,
			char *msg)
{
	if (s != NULL)
		seq_printf(s, "%s\n", msg);
	else
		pr_info("%s\n", msg);
}

void inner_zte_prlong_help(struct seq_file *s,
			long lvalue)
{
	if (s != NULL)
		seq_printf(s, "%ld\n", lvalue);
	else
		pr_info("%ld\n", lvalue);
}

/**
 * Print line help function to print in/out stats for
 *     Unmovable, Reclaimable, Movable.
 *     positive for in, minus for out.
 * Suppose same mt mov is impossible.
 * For example, mov from Movable to Movable.
 */
void inner_zte_prline_help_02(struct seq_file *s)
{
	int i, j;
	long unmova, reclaima, mova;
	int m1 = MIGRATE_UNMOVABLE;
	int m2 = MIGRATE_RECLAIMABLE;
	int m3 = MIGRATE_MOVABLE;

	unmova = 0;
	reclaima = 0;
	mova = 0;
	for (i = 0; i < MAX_NR_ZONES; i++)
		for (j = 0; j < MIGRATE_TYPES; ++j) {
			/* to target */
			unmova +=
				page_zte_stats.mov[i][j][m1];
			reclaima +=
				page_zte_stats.mov[i][j][m2];
			mova +=
				page_zte_stats.mov[i][j][m3];

			/* from target */
			unmova -=
				page_zte_stats.mov[i][m1][j];
			reclaima -=
				page_zte_stats.mov[i][m2][j];
			mova -=
				page_zte_stats.mov[i][m3][j];
		}

	if (s != NULL)
		seq_printf(s,
			"%ld, %ld, %ld",
			unmova,
			reclaima,
			mova);
	else
		pr_info("%ld, %ld, %ld",
			unmova,
			reclaima,
			mova);

	inner_zte_page_stat_newline_print(s);
}

/**
 * Print line help function to print one line information,
 *     so as to prevent code format violation
 *     of the long line warning.
 *     The caller must make sure the valid range for each params.
 * @param s: the print handle of seq file.
 * @param zidx: zone index.
 * @param msrc: which the page block move from.
 * @param mtgt: which the page block move to.
 */
void inner_zte_prline_help_01(struct seq_file *s,
				int zidx,
				int msrc,
				int mtgt)
{
	if (s != NULL)
		seq_printf(s,
			"%8s MOV: %8s to %8s, %ld",
			local_z_names[zidx],
			local_m_names[msrc],
			local_m_names[mtgt],
			page_zte_stats.mov[zidx][msrc][mtgt]);
	else
		pr_info("%8s MOV: %8s to %8s, %ld",
			local_z_names[zidx],
			local_m_names[msrc],
			local_m_names[mtgt],
			page_zte_stats.mov[zidx][msrc][mtgt]);

	inner_zte_page_stat_newline_print(s);
}

void inner_zte_page_stat_migrate_mov_print(struct seq_file *s)
{
	int i, j, k;
	long tm = 0; /* total mov count */

	for (i = 0; i < MAX_NR_ZONES; i++)
		for (j = 0; j < MIGRATE_TYPES; ++j)
			for (k = 0; k < MIGRATE_TYPES;
				k++) {
				if (page_zte_stats.mov[i][j][k]
					== 0) /* skip 0 items */
					continue;

				if (j == k)
					inner_zte_prerror_help(s,
						"warn: same mt move");

				inner_zte_prline_help_01(s,
					i, j, k);
				tm +=
					page_zte_stats.mov[i][j][k];
			}

	/* print other info we care about */
	inner_zte_prline_help_02(s);
	inner_zte_prlong_help(s, tm);
}

/**
 * Print the stat item info.
 * used when printk or seq_file(s is not null) interface.
 */
int inner_zte_page_stat_orderline_print(struct seq_file *s,
			struct page_zte_stat_item_type *item,
			unsigned int zindex,
			unsigned int mtype)
{
	unsigned int order;
	long count;

	if (!item) /* skip null pages */
		return 0;

	for (order = 0; order < MAX_ORDER; ++order) {
		count = atomic_long_read(
			&item->demand[zindex][mtype][order]);
		if (s != NULL)
			seq_printf(s, "%6ld ",	count);
		else
			pr_info("%6ld ", count);

		inner_page_zte_stats_gla_add(
			count, order);
		zte_page_stats_gl_add(count * (1 << order));
	}
	inner_zte_page_stat_newline_print(s);

	return 0;
}

/**
 * Print the stat item info.
 * used when printk or seq_file(s is not null) interface.
 */
int inner_zte_page_stat_item_info_print(struct seq_file *s,
			struct page_zte_stat_item_type *item)
{
	unsigned int order, zindex, mtype;

	if (!item) /* skip null pages */
		return 0;

	/* Print header */
	if (s != NULL) {
		seq_printf(s, "%-35s %7ld ", "pzs item",
			atomic_long_read(&item->pages));
		for (order = 0; order < MAX_ORDER; ++order)
			seq_printf(s, "%6d ", order);
	} else {
		pr_info("%-35s %7ld ", "pzs item",
			atomic_long_read(&item->pages));
		for (order = 0; order < MAX_ORDER; ++order)
			pr_info("%6d ", order);
	}
	inner_zte_page_stat_newline_print(s);

	zte_page_stats_gl_reset();
	inner_page_zte_stats_gla_reset();
	for (zindex = 0; zindex < MAX_NR_ZONES; ++zindex) {
		for (mtype = 0; mtype < MIGRATE_TYPES; mtype++) {
			if (s != NULL)
				seq_printf(s, "Node %4d, zone %8s, type %12s ",
					0, /* suppose fixed only one node */
					local_z_names[zindex],
					local_m_names[mtype]);
			else
				pr_info("Node %4d, zone %8s, type %12s ",
					0, /* suppose fixed only one node */
					local_z_names[zindex],
					local_m_names[mtype]);

			inner_zte_page_stat_orderline_print(
				s, item, zindex, mtype);
		}
	}

	if (s != NULL) {
		seq_printf(s, "%-35s %7ld ", "total",
			zte_page_stats_gl_get());
	} else {
		pr_info("%-35s %7ld ", "total",
			zte_page_stats_gl_get());
	}
	inner_page_zte_stats_gla_print(s);
	inner_zte_page_stat_newline_print(s);

	return 0;
}

/**
 * Print the stats info.
 * used when printk or seq_file(s is not null) interface.
 * ATTENTION: as we don't use the mutex, so it may not consistent
 * between the data outputs. But it doesn't matter.
 */
int zte_page_stats_info_print(struct seq_file *s)
{
	/* not working */
	if (!zte_page_stats_in_working()) {
		if (s != NULL)
			seq_printf(s,
				"pzs not working %d, flags=0x%x\n",
				inner_pzs_get_state(),
				inner_pzs_get_errorflag());
		else
			pr_info("pzs not working %d, flags=0x%x\n",
				inner_pzs_get_state(),
				inner_pzs_get_errorflag());
		return 0;
	}

	/* working */
	if (s != NULL)
		seq_printf(s,
			"pzs pgs %ld, %ld, %ld, %ld\n",
			atomic_long_read(&page_zte_stats.cur.pages),
			atomic_long_read(&page_zte_stats.min.pages),
			atomic_long_read(&page_zte_stats.max.pages),
			global_page_state(NR_FREE_PAGES));
	else
		pr_info(
			"pzs pgs %ld, %ld, %ld, %ld\n",
			atomic_long_read(&page_zte_stats.cur.pages),
			atomic_long_read(&page_zte_stats.min.pages),
			atomic_long_read(&page_zte_stats.max.pages),
			global_page_state(NR_FREE_PAGES));

	/* we care the demand parts */
	if (s != NULL)
		seq_printf(s,
			"pzs cur:\n");
	else
		pr_info("pzs cur:\n");
	inner_zte_page_stat_item_info_print(s,
		&page_zte_stats.cur);
	/* delimiter line between cur and max */
	inner_zte_page_stat_newline_print(s);

	if (s != NULL)
		seq_printf(s,
			"pzs max:\n");
	else
		pr_info("pzs max:\n");
	inner_zte_page_stat_item_info_print(s,
		&page_zte_stats.max);

	/* delimiter line between cur and max */
	inner_zte_page_stat_newline_print(s);
	inner_zte_page_stat_migrate_mov_print(s);

	return 0;
}

/**
 * alloc stats: not check the params for we trust page_alloc module,
 *     and don't print anything for may called in critical areas.
 * @param migratetype: which migratetype got.
 * @param high_zoneidx: which zone.
 * @param page: returned, maybe null.
 * @param order: which order page.
 * @param alloc: 1 for yes, 0 for free.
 * @return: value of page_zte_stats_error_flag int.
 */
int zte_page_stats_update(int migratetype, enum zone_type high_zoneidx,
			struct page *page, unsigned int order, int alloc)
{
	int ret = STATS_ERROR_NO_ERROR;
	long pages;
	atomic_long_t *pl;

	/* only work when no error and initialized */
	if (!page) /* else { we ignore the null case } */
		return ret;

	/* only dma and normal used */
	if (high_zoneidx > ZONE_MOVABLE
		|| high_zoneidx < ZONE_DMA) {
		ret = inner_pzs_set_errorflag(
			STATS_ERROR_ZONEIDX_INVALID);
		return ret;
	}

	pages = 1 << order;
	pl = &page_zte_stats.cur.demand[high_zoneidx][migratetype][order];
	if (alloc) {
		atomic_long_inc(pl);
		atomic_long_add(pages,
			&page_zte_stats.cur.pages);
	} else { /* free */
		atomic_long_dec(pl);
		atomic_long_sub(pages,
			&page_zte_stats.cur.pages);
	}

	/* data structure copied if max happened(may not consistent) */
	if (atomic_long_read(&page_zte_stats.cur.pages) >
		atomic_long_read(&page_zte_stats.max.pages))
		page_zte_stats.max = page_zte_stats.cur;

	/* data structure copied if min happened */
	if (atomic_long_read(&page_zte_stats.cur.pages) <
		atomic_long_read(&page_zte_stats.min.pages))
		page_zte_stats.min = page_zte_stats.cur;

	return STATS_ERROR_NO_ERROR;
}

/**
 * adjust stats: just a different name as zte_page_stats_update
 * @param migratetype: which migratetype got when adjust.
 * @param zoneidx: which zone.
 * @param page: returned, maybe null.
 * @param order: which order page adjusted.
 * @param alloc: 1 for yes, 0 for free.
 * @return: error flags for error, 0 for normal.
 */
int zte_page_stats_adjust(int migratetype, enum zone_type zoneidx,
			struct page *page, unsigned int order, int alloc)
{
	return zte_page_stats_update(migratetype,
		zoneidx, page, order, alloc);
}

/**
 * once time init when kernel mm startup,
 * before __alloc_pages_nodemask is used.
 */
int zte_page_stats_init(void)
{
	memset(&page_zte_stats, 0,
	       sizeof(struct page_zte_stats_type));

	/* actually this user param control has the default value */
	pzs_enable = PZS_ENABLE;

	/* leave a place to init when start up */
	inner_pzs_reset_state(); /* reset */
	return 0;
}


