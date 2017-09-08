#ifndef __MM_PAGE_ZTE_MT_PREMOV_H__
#define __MM_PAGE_ZTE_MT_PREMOV_H__

bool is_pzmp_unmovable_cma(struct cma *cma);
int pzmp_get_cma_migratetype(struct cma *cma);
void pzmp_contiguous_reserve(void);

#endif
