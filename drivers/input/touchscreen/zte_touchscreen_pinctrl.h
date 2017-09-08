
#ifndef _ZTE_TOUCH_PINCTRL_H
#define _ZTE_TOUCH_PINCTRL_H


typedef struct zte_pinctrl_info {
	struct device *dev;
	struct pinctrl *ts_pinctrl;
	char *active_describe;
	char *suspend_describe;
	char *release_describe;
	struct pinctrl_state *ts_state_active;
	struct pinctrl_state *ts_state_suspend;
	struct pinctrl_state *ts_state_release;
} ZTE_PINCTRL_INFO_T;

int zte_ts_pinctrl_init(ZTE_PINCTRL_INFO_T *pinctrl_node);
int zte_ts_pinctrl_configure(ZTE_PINCTRL_INFO_T *pinctrl_node,
			     struct pinctrl_state *set_state);
int zte_ts_pinctrl_ralease(ZTE_PINCTRL_INFO_T *pinctrl_node);

#endif

