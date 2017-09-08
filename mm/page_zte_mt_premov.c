/*
 * Contiguous Memory Allocator
 *
 * Copyright (c) 2010-2011 by Samsung Electronics.
 * Copyright IBM Corporation, 2013
 * Copyright LG Electronics Inc., 2014
 * Written by:
 *	Marek Szyprowski <m.szyprowski@samsung.com>
 *	Michal Nazarewicz <mina86@mina86.com>
 *	Aneesh Kumar K.V <aneesh.kumar@linux.vnet.ibm.com>
 *	Joonsoo Kim <iamjoonsoo.kim@lge.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License or (at your optional) any later version of the license.
 */

#define pr_fmt(fmt) "pzmp: " fmt

#ifdef CONFIG_PZMP_DEBUG
#ifndef DEBUG
#  define DEBUG
#endif
#endif
#define CREATE_TRACE_POINTS

#include <linux/memblock.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/log2.h>
#include <linux/cma.h>
#include <linux/highmem.h>
#include <linux/delay.h>
#include <linux/kmemleak.h>

#include <linux/dma-contiguous.h>
#include "cma.h"
#include "page_zte_mt_premov.h"

/* to shorten the length of symbol name, use PZMP for */
/* abbreviation of page zte mt premov */
/* We don't changed the behaviour of DMA, which used by kernel */

/* both 300M, should be multiple of max(MAX_ORDER - 1, pageblock_order) */
#define RECLAIMABLE_NROMAL_SIZE 0x2000000 /* 32M */
#define UNMOVABLE_NROMAL_SIZE 0x10000000 /* 256M */
#define PREMOV_UNIT_SIZE 0x400000 /* 4M=pageblock_order */

/* refer to max_zone_dma_phys in init.c of arm64-mm */
#define PZMP_DMA_LIMIT 0xffffffff

struct cma *normal_reclaimable_premov_area;
struct cma *normal_unmobable_premov_area;

bool is_pzmp_unmovable_cma(struct cma *cma)
{
	return normal_reclaimable_premov_area == cma ||
		normal_unmobable_premov_area == cma;
}

int pzmp_get_cma_migratetype(struct cma *cma)
{
	int mt = MIGRATE_CMA; /* the original value used */

	/* changed the migratetype for specific purpose */
	/* the _premov_area may not be used and is NULL */
	if (normal_unmobable_premov_area == cma)
		mt = MIGRATE_UNMOVABLE;
	else if (normal_reclaimable_premov_area == cma)
		mt = MIGRATE_RECLAIMABLE;

	return mt;
}

/**
 * pzmp_contiguous_reserve() - reserve area(s) for unmovable premov handling
 * This function reserves memory from early allocator. It should be
 * called by arch specific code once the early allocator (memblock or bootmem)
 * has been activated and all other subsystems have already allocated/reserved
 * memory.
 * Please refer to zone_sizes_init for the boundary
 * between DMA zone and normal zone, which decide the params below.
 * no need to create kernel map compared with the standard cma.
 */
void __init pzmp_contiguous_reserve(void)
{
	/* reserve memory for unmovable dma */
	if (!normal_reclaimable_premov_area
		&& RECLAIMABLE_NROMAL_SIZE >= PREMOV_UNIT_SIZE) {
		cma_declare_contiguous(
			PZMP_DMA_LIMIT + 1, /* zone normal */
			RECLAIMABLE_NROMAL_SIZE,
			0, /* limit */
			0, 0, /* alignment and order_per_bit */
			false, /* fixed */
			&normal_reclaimable_premov_area);
		if (!normal_reclaimable_premov_area)
			pr_err("normal reclaimable alloc failed\n");
		else
			pr_info("reclaimable prealloc %lu\n",
				cma_bitmap_maxno(
				normal_reclaimable_premov_area));
	}

	/* reserve memory for unmovable normal */
	if (!normal_unmobable_premov_area
		&& UNMOVABLE_NROMAL_SIZE >= PREMOV_UNIT_SIZE) {
		cma_declare_contiguous(
			PZMP_DMA_LIMIT + 1, /* base in zone normal */
			UNMOVABLE_NROMAL_SIZE,
			0, /* limit to the end of DRAM */
			0, 0, /* alignment and order_per_bit */
			false, /* fixed */
			&normal_unmobable_premov_area);
		if (!normal_unmobable_premov_area)
			pr_err("normal unmovable alloc failed\n");
		else
			pr_info("unmovable prealloc %lu\n",
				cma_bitmap_maxno(
				normal_unmobable_premov_area));
	}
}

