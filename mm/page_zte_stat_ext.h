/* internal.h: mm/ internal definitions
 *
 * Copyright (C) 2004 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#ifndef __MM_PAGE_ZTE_STAT_EXT_H
#define __MM_PAGE_ZTE_STAT_EXT_H

void zte_page_stats_gl_reset(void);
void zte_page_stats_gl_add(long delta);
long zte_page_stats_gl_get(void);

/**
 * the below 3 will return page_zte_stats_error_flag int.
 * but user should take 0 as normal, others as error.
 */
int zte_page_stats_move_freepages(struct zone *zone,
			unsigned long pages_count,
			int migratetype, int old_mt);
int zte_page_stats_update(int migratetype, enum zone_type high_zoneidx,
			struct page *page, unsigned int order, int alloc);
int zte_page_stats_adjust(int migratetype, enum zone_type zoneidx,
			struct page *page, unsigned int order, int alloc);

int zte_page_stats_info_print(struct seq_file *s);
int zte_page_stats_in_working(void);
int zte_page_stats_init(void);

#endif	/* __MM_PAGE_ZTE_STAT_EXT_H */
