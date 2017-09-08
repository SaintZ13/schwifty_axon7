#ifndef __MM_PAGE_ZTE_PRIO_APP_H__
#define __MM_PAGE_ZTE_PRIO_APP_H__

/* it equals to the PROP_VALUE_MAX where app data come from */
#define PRIO_APP_BUFFER_LEN	92
extern char zte_prio_apps[PRIO_APP_BUFFER_LEN];

int is_pzpa_debug_enabled(void);
int is_pzpa_normal_dbg_enabled(void);
int is_pzpa_zoneinfo_dbg_enabled(void);
int is_pzpa_zprio_lru_dbg_enabled(void);

int is_pzpa_prio_apps(const char *buf);
int is_pzpa_vma_prio_flags(struct vm_area_struct *vma);
void pzpa_clear_zprio_flags(struct page *page);
int pzpa_cc_zprio_flags(struct page *page); /* check and clear */

void pzpa_println_filename_from_as(struct address_space *mapping);
void pzpa_println_filename_from_file(struct file *file);

/* beginning of the control part 1/2 */
/* input from 0 to 100, and the denominator is double = 200, */
/*	so at most 50% defined here */
#define PZPA_INACT_LOW_RATIO_MAX 100
#define PZPA_INACT_LOW_DENOM 200 /* double above max */
#define PZPA_MAX_SCAN_MULTIPLE 2 /* input from 1 to 2 */

#define PZPA_LIGHT_LOOP_LIMIT 100 /* full calc every 100 loops */
#define PZPA_DEFAULT_FACTOR_NR 10 /* 1/10 zlur in active lur */

#define PZPA_FULL_CALC 0 /* parameter for full calc */
#define PZPA_LIGHT_JUDGE 1 /* parameter for light judge */

int is_pzpa_control_enabled(void);
int is_pzpa_zprio_lru_dbg_enabled(void);
int is_pzpa_zprio_under_limit(void);
unsigned long pzpa_get_ifl_factor(int light);
int is_pzpa_zprio_lru_keep(void); /* full calc */
int is_light_pzpa_zprio_lru_keep(void); /* light judge */
int pzpa_get_zprio_lru_scan_factor(void);
int pzpa_update_add_stats(enum zone_stat_item lru, struct page *page);
int pzpa_update_del_stats(enum zone_stat_item lru, struct page *page);

#endif
