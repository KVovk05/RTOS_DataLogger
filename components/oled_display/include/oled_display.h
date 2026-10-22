#pragma once
#include "rtc_ds1307.h"

#ifdef __cplusplus
extern "C" {
#endif

void oled_display_init(void);
void oled_play_wolf_boot_animation(void);
void oled_draw_dashboard(const rtc_time_t *t, 
                         float temp_c, float hum_rh, float press_mm, 
                         const char *status_msg);

#ifdef __cplusplus
}
#endif