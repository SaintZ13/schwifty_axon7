/*
 * zte priority application handler
 *
 * Copyright (c) 2010-2011 by Zte Corp.
 * Copyright IBM Corporation, 2013
 * Copyright LG Electronics Inc., 2014
 * See linux/MAINTAINERS for address of current maintainer.
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License or (at your optional) any later version of the license.
 */

#define pr_fmt(fmt) "pzpa: " fmt

#ifdef CONFIG_PZPA_DEBUG
#ifndef DEBUG
#  define DEBUG
#endif
#endif
#define CREATE_TRACE_POINTS

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/vmpressure.h>
#include <linux/vmstat.h>
#include <linux/file.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/mm_inline.h>
#include <linux/backing-dev.h>
#include <linux/rmap.h>
#include <linux/topology.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/compaction.h>
#include <linux/notifier.h>
#include <linux/rwsem.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/memcontrol.h>
#include <linux/delayacct.h>
#include <linux/sysctl.h>
#include <linux/oom.h>
#include <linux/prefetch.h>
#include <linux/printk.h>
#include <linux/debugfs.h>

#include <asm/tlbflush.h>
#include <asm/div64.h>

#include <linux/swapops.h>
#include <linux/balloon_compaction.h>

#include "internal.h"

#include "page_zte_prio_app.h"

#define PRIO_APP_NAME_LEN_MIN 5
/* initially no app set */
char zte_prio_apps[PRIO_APP_BUFFER_LEN] = "";

/* beginning of the control part 1 */
static int inner_pzpa_initialized;
struct mutex pzpa_control_mutex;

#define PZPA_CNTL_LIMIT_MIN 0
#define PZPA_CNTL_LIMIT_MAX 50000 /* nr pages */
static int pzpa_min_set(const char *val, struct kernel_param *kp);
static int pzpa_med_set(const char *val, struct kernel_param *kp);
static int pzpa_high_set(const char *val, struct kernel_param *kp);

static int pzpa_ilr_set(const char *val, struct kernel_param *kp);

static int pzpa_msr_set(const char *val, struct kernel_param *kp);

#define PZPA_DEGRADE_LOOP 10000 /* input from 1 to 10000 */
static int pzpa_dgdlp_set(const char *val, struct kernel_param *kp);

#define PZPA_DEFAULT_CNTL_LIMIT_MIN 30000
#define PZPA_DEFAULT_CNTL_LIMIT_MED 50000
#define PZPA_DEFAULT_CNTL_LIMIT_MAX 100000
#define PZPA_DEFAULT_INACT_LOW_RATIO_MAX 50 /* 1/4 */
#define PZPA_DEFAULT_MAX_SCAN_MULTIPLE 2 /* scan 64 instead of 32 pages */
#define PZPA_DEFAULT_DEGRADE_LOOP 1000 /* condition for control limit change */

static long pzpa_nr_min; /* nr_pages */
static long pzpa_nr_med; /* nr_pages */
static long pzpa_nr_high; /* nr_pages */

/* current zprio limit and print it whenever changed */
static long pzpa_curzp_limit;
static long pzpa_rough_peak; /* nr_pages */

static int pzpa_inact_low_ratio; /* used when shrink list */
static int pzpa_max_scan_ratio; /* used when shrink scan */
static long pzpa_degrade_loop; /* nr_loops */
module_param_call(limit_min, pzpa_min_set, param_get_long,
			&pzpa_nr_min, 0644);
module_param_call(limit_med, pzpa_med_set, param_get_long,
			&pzpa_nr_med, 0644);
module_param_call(limit_high, pzpa_high_set, param_get_long,
			&pzpa_nr_high, 0644);

/* use acronyms as function name, ilr = inact low ratio */
module_param_call(inact_low_ratio, pzpa_ilr_set, param_get_int,
			&pzpa_inact_low_ratio, 0644);
module_param_call(max_scan_ratio, pzpa_msr_set, param_get_int,
			&pzpa_max_scan_ratio, 0644);

module_param_call(degrade_loop, pzpa_dgdlp_set, param_get_long,
			&pzpa_degrade_loop, 0644);

enum {
	PZPA_CONTROL_DISABLE = 0x0, /* disable prio control */
	PZPA_CONTROL_ENABLE = 0x1, /* enable by default */
};
static int pzpa_control_enable; /* print whenever changed */
static int pzpa_cntl_en_set(const char *val, struct kernel_param *kp);
module_param_call(control_enable, pzpa_cntl_en_set, param_get_int,
			&pzpa_control_enable, 0644);
/**
 * Now only allowed from ENABLE to DISABLE.
 * @return: -EPERM for not allowed, 0 for normal.
 */
static int pzpa_cntl_en_set(const char *val, struct kernel_param *kp)
{
	int ret;

	if (!inner_pzpa_initialized)
		return -EAGAIN;

	/* always set because true or false always valid with a int */
	ret = param_set_int(val, kp);
	if (ret) /*unexpected param */
		return ret;

	pr_info("pzpa control enable %d, peak=%ld\n",
		pzpa_control_enable,
		pzpa_rough_peak);

	return 0;
}
/**
 * check the validity of user control limit.
 * the input value is the number of pages under control
 * the mutex is only for mutual exclusion between
 * several parameter settings(min, med, high), not for users
 */
static int pzpa_min_set(const char *val, struct kernel_param *kp)
{
	int ret;
	long old_val = pzpa_nr_min;

	if (!inner_pzpa_initialized)
		return -EAGAIN;

	ret = param_set_long(val, kp);
	if (ret) /*unexpected param */
		return ret;

	ret = -EINVAL; /* default as invalid parameter */
	if (pzpa_nr_min >= PZPA_CNTL_LIMIT_MIN &&
		pzpa_nr_min <= PZPA_CNTL_LIMIT_MAX) {
		/* try to make sure the validity */
		mutex_lock(&pzpa_control_mutex);
		/* check the correlation, med and high >= min */
		if (pzpa_nr_min <= pzpa_nr_med
			&& pzpa_nr_min <= pzpa_nr_high)
			ret = 0; /* success */
		mutex_unlock(&pzpa_control_mutex);
	}

	if (ret) {
		pr_info("pzpa min invalid %ld\n",
			pzpa_nr_min);
		pzpa_nr_min = /* restore the old value*/
			old_val;
	} else
		pr_info("pzpa min set %ld\n",
			pzpa_nr_min);

	return 0;
}

/**
 * check the validity of user control limit.
 * the input value is the number of pages under control
 */
static int pzpa_med_set(const char *val, struct kernel_param *kp)
{
	int ret;
	long old_val = pzpa_nr_med;

	if (!inner_pzpa_initialized)
		return -EAGAIN;

	ret = param_set_long(val, kp);
	if (ret) /*unexpected param */
		return ret;

	ret = -EINVAL; /* default as invalid parameter */
	if (pzpa_nr_med >= PZPA_CNTL_LIMIT_MIN &&
		pzpa_nr_med <= PZPA_CNTL_LIMIT_MAX) {
		/* try to make sure the validity */
		mutex_lock(&pzpa_control_mutex);
		/* check the correlation, med and high >= min */
		if (pzpa_nr_min <= pzpa_nr_med
			&& pzpa_nr_med <= pzpa_nr_high)
			ret = 0; /* success */
		mutex_unlock(&pzpa_control_mutex);
	}

	if (ret) {
		pr_info("pzpa med invalid %ld\n",
			pzpa_nr_med);
		pzpa_nr_med = /* restore the old value*/
			old_val;
	} else
		pr_info("pzpa med set %ld\n",
			pzpa_nr_med);

	return 0;
}

/**
 * check the validity of user control limit.
 * the input value is the number of pages under control
 */
static int pzpa_high_set(const char *val, struct kernel_param *kp)
{
	int ret;
	long old_val = pzpa_nr_high;

	if (!inner_pzpa_initialized)
		return -EAGAIN;

	ret = param_set_long(val, kp);
	if (ret) /*unexpected param */
		return ret;

	ret = -EINVAL; /* default as invalid parameter */
	if (pzpa_nr_high >= PZPA_CNTL_LIMIT_MIN &&
		pzpa_nr_high <= PZPA_CNTL_LIMIT_MAX) {
		/* try to make sure the validity */
		mutex_lock(&pzpa_control_mutex);
		/* check the correlation, med and high >= min */
		if (pzpa_nr_min <= pzpa_nr_high
			&& pzpa_nr_med <= pzpa_nr_high) {
			/* update cur limit if high decreased */
			if (pzpa_nr_high <
				pzpa_curzp_limit) {
				pzpa_curzp_limit =
					pzpa_nr_high;
				pr_info("pzpa cur limit chged %ld\n",
					pzpa_curzp_limit);
			}
			ret = 0; /* success */
		}
		mutex_unlock(&pzpa_control_mutex);
	}

	if (ret) {
		pr_info("pzpa high invalid %ld\n",
			pzpa_nr_high);
		pzpa_nr_high = /* restore the old value*/
			old_val;
	} else
		pr_info("pzpa high set %ld\n",
			pzpa_nr_high);

	return 0;
}

/**
 * check the validity of user control limit.
 * the input value is 0 - 100 of ratio number
 */
static int pzpa_ilr_set(const char *val, struct kernel_param *kp)
{
	int ret;
	int old_val = pzpa_inact_low_ratio;

	if (!inner_pzpa_initialized)
		return -EAGAIN;

	ret = param_set_int(val, kp);
	if (ret) /*unexpected param */
		return ret;

	if (pzpa_inact_low_ratio < 0 ||
		pzpa_inact_low_ratio
		> PZPA_INACT_LOW_RATIO_MAX) {
		/* no need mutex exclusion */
		pr_info("pzpa ilr invalid %d\n",
			pzpa_inact_low_ratio);
		pzpa_inact_low_ratio = /* restore the old value*/
			old_val;
	} else
		pr_info("pzpa ilr set %d\n",
			pzpa_inact_low_ratio);

	return 0;
}

/**
 * check the validity of user control limit.
 * the input value is 1 - 2 of ratio number
 */
static int pzpa_msr_set(const char *val, struct kernel_param *kp)
{
	int ret;
	int old_val = pzpa_max_scan_ratio;

	if (!inner_pzpa_initialized)
		return -EAGAIN;

	ret = param_set_int(val, kp);
	if (ret) /*unexpected param */
		return ret;

	if (pzpa_max_scan_ratio < 1 ||
		pzpa_max_scan_ratio
		> PZPA_MAX_SCAN_MULTIPLE) {
		/* no need mutex exclusion */
		pr_info("pzpa msr invalid %d\n",
			pzpa_max_scan_ratio);
		pzpa_max_scan_ratio = /* restore the old value*/
			old_val;
	} else
		pr_info("pzpa msr set %d\n",
			pzpa_max_scan_ratio);

	return 0;
}

/**
 * check the validity of user control limit.
 * the input value is the number of pages under control
 */
static int pzpa_dgdlp_set(const char *val, struct kernel_param *kp)
{
	int ret;
	long old_val = pzpa_degrade_loop;

	if (!inner_pzpa_initialized)
		return -EAGAIN;

	ret = param_set_long(val, kp);
	if (ret) /*unexpected param */
		return ret;

	if (pzpa_degrade_loop < 1 ||
		pzpa_degrade_loop
		> PZPA_DEGRADE_LOOP) {
		/* no need mutex exclusion */
		pr_info("pzpa dgdlp invalid %ld\n",
			pzpa_degrade_loop);
		pzpa_degrade_loop = /* restore the old value*/
			old_val;
	} else
		pr_info("pzpa dgdlp set %ld\n",
			pzpa_degrade_loop);

	return 0;
}
/* end of the control part 1 */

/* mask bit defined below */
enum PZPA_DEBUG_MASK {
	PZPA_DBG_NORMAL	= BIT(0),
	PZPA_DBG_ZONEINFO	= BIT(1),
	PZPA_DBG_ZPRIO_LRU	= BIT(2),
	PZPA_DBG_ANY	        = 0xffffffff  /* enable all logs */
};

static unsigned int pzpa_debug_mask; /* default is zero */
module_param_named(
	debug_mask, pzpa_debug_mask, uint, S_IRUSR | S_IWUSR
);

/**
 * is_pzpa_*_enabled
 * check whether pzpa debug is opened.
 * @return: 0 for not, while 1 for yes.
 */
int is_pzpa_debug_enabled(void)
{
	return pzpa_debug_mask;
}
int is_pzpa_normal_dbg_enabled(void)
{
	return pzpa_debug_mask & PZPA_DBG_NORMAL;
}
int is_pzpa_zoneinfo_dbg_enabled(void)
{
	return pzpa_debug_mask & PZPA_DBG_ZONEINFO;
}
int is_pzpa_zprio_lru_dbg_enabled(void)
{
	return pzpa_debug_mask & PZPA_DBG_ZPRIO_LRU;
}

/**
 * is_pzpa_prio_apps
 * check whether input comm(buf) belongs to predefined top apps.
 * @param buf: thread comm parameter.
 * @return: 0 for not contained or buf is null, otherwize (1) for yes.
 */
int is_pzpa_prio_apps(const char *buf)
{
	size_t len = strlen(zte_prio_apps);

	/* unexpected len check, only once of this module */
	if (len > PRIO_APP_BUFFER_LEN - 1) {
		pr_warn_once("top app invalid len: \"%s\"\n",
			zte_prio_apps);
		return 0;
	}

	if (strlen(buf) >= PRIO_APP_NAME_LEN_MIN
		&& strnstr(zte_prio_apps, buf,
		len))
		return 1;
	else /* unmatched */
		return 0;
}

/**
 * is_pzpa_vma_prio_flags
 * check if vma has prio app flags.
 * @param vma: the target page belong to.
 * @return: 0 for not prio app, otherwize (1) for yes.
 */
int is_pzpa_vma_prio_flags(struct vm_area_struct *vma)
{
	struct mm_struct *mm;

	if (vma) {
		mm = vma->vm_mm;
		if (mm) {
			if (test_bit(MMF_ZTE_PRIO_APP, &mm->flags))
				return 1;
		}
	}

	/* in default case except above check */
	return 0;
}

/**
 * pzpa_println_filename_from_as
 * get the file name which corresponding to a file,
 * and print it
 * @param mapping: the embedded struct in inode.
 * @return: not used.
 */
void pzpa_println_filename_from_as(struct address_space *mapping)
{
	unsigned long	nrpages, nrshadows;
	struct inode	*inode;
	struct hlist_node *hnode;
	struct dentry *dentry;

	/* debug control */
	if (!is_pzpa_normal_dbg_enabled())
		return;

	if (mapping) {
		nrpages = mapping->nrpages;
		nrshadows = mapping->nrshadows;
		inode = mapping->host;
		if (inode) {
			hnode = inode->i_dentry.first;
			if (hnode) { /* &dentry->d_u.d_alias */
				dentry =
					hlist_entry_safe(hnode,
					typeof(*(dentry)), d_u.d_alias);
				if (dentry) {
					pr_info("top app detach \"%s\", %u, %u\n",
						dentry->d_iname,
						dentry->d_lockref.count,
						inode->i_nlink);
					return;
				}
			}
		}
	}

	/* default case */
	pr_info("INVALID mapping\n");
}

/**
 * pzpa_println_filename_from_file
 * get the file name which corresponding to a file,
 * and print it
 * @param file: the struct representing a file.
 * @return: not used.
 */
void pzpa_println_filename_from_file(struct file *file)
{
	struct dentry *dentry;

	/* debug control */
	if (!is_pzpa_normal_dbg_enabled())
		return;

	if (file) {
		dentry = file->f_path.dentry;
		if (dentry) {
			pr_info("top app set \"%s\", %u\n",
				dentry->d_iname,
				dentry->d_lockref.count);
			return;
		}
	}

	/* default case */
	pr_info("INVALID file\n");
}

/**
 * the caller must make sure page is not null and
 * may or may not be in a LRU list.
 */
void pzpa_clear_zprio_flags(struct page *page)
{
	__dec_zone_page_state(page, NR_ZPRIO);
	__ClearPageZprio(page);
}

/**
 * the caller must make sure page is not null and
 * may or may not be in a LRU list.
 * use the two-letter acronym of cc for check and clear
 * int value as true for zprio stats changed or false for nothing.
 */
int pzpa_cc_zprio_flags(struct page *page)
{
	int ret = 0;

	if (PageZprio(page)) {
		pzpa_clear_zprio_flags(page);
		ret = 1;
	}

	return ret;
}

/* beginning of the control part 2 */
/**
 * if feature enabled.
 * take effect immediately.
 * int value as true or false
 */
int is_pzpa_control_enabled(void)
{
	return pzpa_control_enable;
}

/**
 * the caller must make sure page is not null and in a LRU list.
 * used when add page to a lru
 * @param lru: NR type of stat enum.
 * int value as true for stats changed or false for nothing changed
 */
int pzpa_update_add_stats(enum zone_stat_item lru, struct page *page)
{
	int ret = 0;

#ifdef CONFIG_ZTE_PRIO_APP_DEBUG
	/* already got the tag: unexpected debug */
	if (PageZplru(page)) {
		__inc_zone_page_state(
			page, NR_ZDBG0);
	}
#endif

	if (PageZprio(page)) {
		if (!PageZplru(page)) { /* new lru for zprio */
			__SetPageZplru(page);
			__inc_zone_page_state(
				page, NR_LRU_ZPRIO);
			ret++;

			if (lru ==
				NR_ACTIVE_FILE) {
				__inc_zone_page_state(
					page, NR_ZPRIO_ALRU);
				ret++;
			}
		}

		if (!is_pzpa_control_enabled()) {
			/* untag zprio, but leave zplru and alru stats */
			pzpa_clear_zprio_flags(page);
			ret++;
		}
	}

	return ret;
}

/**
 * the caller must make sure page is not null.
 * ATTENTION: the target page may or may not has ZPRIO tag.
 * called when page is deleted from LRU, but may add back later,
 * and the LRU tag may have been cleared.
 * this function will not change the zprio flag, except disable check.
 * @param lru: NR type of stat enum.
 * int value as true for stats changed or false for nothing changed
 */
int pzpa_update_del_stats(enum zone_stat_item lru, struct page *page)
{
	int ret = 0;

	if (PageZplru(page)) {
		__ClearPageZplru(page);
		__dec_zone_page_state(
			page, NR_LRU_ZPRIO);
		ret++;

		if (lru ==
			NR_ACTIVE_FILE) {
			__dec_zone_page_state(
				page, NR_ZPRIO_ALRU);
			ret++;
		}
	}

	if (PageZprio(page)) {
		if (!is_pzpa_control_enabled()) {
			pzpa_clear_zprio_flags(page);
			ret++;
		}
	}

	/* default */
	return ret;
}

/**
 * current zprio numbers over limit check.
 * and we take sample for peak zprio stat here
 * true for under limit or false for over limit
 */
int is_pzpa_zprio_under_limit(void)
{
	unsigned long rough_figure;

	if (!is_pzpa_control_enabled())
		return 0; /* over limit */

	rough_figure =
		global_page_state(NR_ZPRIO_ALRU);
	if (pzpa_rough_peak < rough_figure)
		pzpa_rough_peak = rough_figure;
	return rough_figure <= pzpa_curzp_limit;
}

/**
 * ifl = inactive file low.
 * used to influnce inactive_file_is_low with zprio.
 * @param light: control if full calc(=0) or just light(=1) judge.
 * return non-zero factor tampering active LRU nr, 0 for ignore.
 */
/* The below var is a full calc value of factor nr */
unsigned long pzpa_last_factor_nr = PZPA_DEFAULT_FACTOR_NR;
unsigned long pzpa_get_ifl_factor(int light)
{
	unsigned long zprio_lru, active_file; /* rough value */
	unsigned long factor_nr;

	zprio_lru =
		global_page_state(NR_ZPRIO_ALRU);

	/* status and validity checking, or enough pages */
	if (!is_pzpa_control_enabled() ||
		pzpa_inact_low_ratio <= 0 ||
		pzpa_inact_low_ratio >
			PZPA_INACT_LOW_RATIO_MAX ||
		zprio_lru > pzpa_curzp_limit)
		return 0; /* ignore the effect */

	active_file =
		global_page_state(NR_ACTIVE_FILE);

	/* no zprio lru, so no need to think about it */
	if (zprio_lru == 0) {
		return 0;
	} else if (light == PZPA_LIGHT_JUDGE)
		return pzpa_last_factor_nr;

	/* validity checking */
	if (active_file < zprio_lru ||
		active_file == 0) {
		pr_info("pzpa ifl UNEXPECTED %ld, %ld\n",
			active_file, zprio_lru);
		return 0;
	}

	/* ratio range 0 - 100/double MAX (at most 50%) >
	 *  zprio_lru / total active file, then use ratio factor,
	 * or ignore the factor in active list shrinking.
	 * According to the value ranges,
	 * overflow shouldn't happened here
	 * here to check use how many percentage factor
	*/
	factor_nr = pzpa_inact_low_ratio * active_file; /* as temp var */
	if (factor_nr >
		PZPA_INACT_LOW_DENOM * zprio_lru) {
		/* use the parameter inverse factor */
		factor_nr = active_file / zprio_lru;
	} else /* the current ratio is enough */
		factor_nr = 0;

	pzpa_last_factor_nr = factor_nr; /* update */
	return factor_nr;
}

/**
 * islp = isolate_lru_pages.
 * used to influnce active or inactive list move operation.
 * return if zprio page should be kept or not
 * non-zero for keep, 0 for process as usual.
 */
int is_pzpa_zprio_lru_keep(void)
{
	/* use full calc */
	return pzpa_get_ifl_factor(PZPA_FULL_CALC) > 0;
}

/**
 * islp = isolate_lru_pages.
 * used as same purpose as is_pzpa_zprio_lru_keep,
 * except less calculating, so a light implementation judge.
 * non-zero for keep, 0 for process as usual.
 */
int pzpa_light_loop_limit = PZPA_LIGHT_LOOP_LIMIT;
int is_light_pzpa_zprio_lru_keep(void)
{
	/* use full calc */
	if (pzpa_light_loop_limit >=
		PZPA_LIGHT_LOOP_LIMIT) {
		if (is_pzpa_zprio_lru_dbg_enabled())
			pr_info("pzpa f calc when light judge\n");
		pzpa_light_loop_limit = 0; /* recount */
		return pzpa_get_ifl_factor(PZPA_FULL_CALC) > 0;
	}

	pzpa_light_loop_limit++;
	return pzpa_get_ifl_factor(PZPA_LIGHT_JUDGE) > 0;
}


/**
 * if we skip some zprio page during scan.
 * then we may need to scan more instead of
 * SWAP_CLUSTER_MAX=32
 * the ratio returned is the total nr to be scanned,
 * including the zprio pages.
 */
int pzpa_get_zprio_lru_scan_factor(void)
{
	return pzpa_max_scan_ratio;
}

/**
 * pzpa_init - lazily parameter control init
 *
 * this is only a control part initialization
 * especially used for control parameters setting mutual exclusion.
 * #define PZPA_DEFAULT_CNTL_LIMIT_MIN 0
 * #define PZPA_DEFAULT_CNTL_LIMIT_MAX 50000
 * #define PZPA_DEFAULT_INACT_LOW_RATIO_MAX 100
 * #define PZPA_DEFAULT_MAX_SCAN_MULTIPLE 2
 * #define PZPA_DEFAULT_DEGRADE_LOOP 10000
 */
static int __init pzpa_control_init(void)
{
	mutex_init(&pzpa_control_mutex);

	/* from experience */
	pzpa_nr_min =
		PZPA_DEFAULT_CNTL_LIMIT_MIN;
	pzpa_nr_med =
		PZPA_DEFAULT_CNTL_LIMIT_MED;
	pzpa_nr_high =
		PZPA_DEFAULT_CNTL_LIMIT_MAX;
	pzpa_inact_low_ratio =
		PZPA_DEFAULT_INACT_LOW_RATIO_MAX;
	pzpa_max_scan_ratio =
		PZPA_DEFAULT_MAX_SCAN_MULTIPLE;
	pzpa_degrade_loop =
		PZPA_DEFAULT_DEGRADE_LOOP;

	pzpa_control_enable = PZPA_CONTROL_ENABLE;
	pzpa_curzp_limit = pzpa_nr_high;
	pzpa_rough_peak = 0;
	inner_pzpa_initialized = 1; /* should  put in the last */
	pr_info("pzpa init %ld, en=%d\n",
		pzpa_curzp_limit,
		pzpa_control_enable);
	return 0;
}
late_initcall(pzpa_control_init);
/* end of the control part 2 */
