#ifndef EEZ_LVGL_UI_STYLES_H
#define EEZ_LVGL_UI_STYLES_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Style: status_orange
lv_style_t *get_style_status_orange_MAIN_DEFAULT();
void add_style_status_orange(lv_obj_t *obj);
void remove_style_status_orange(lv_obj_t *obj);

// Style: status_green
lv_style_t *get_style_status_green_MAIN_DEFAULT();
void add_style_status_green(lv_obj_t *obj);
void remove_style_status_green(lv_obj_t *obj);

// Style: status_red
lv_style_t *get_style_status_red_MAIN_DEFAULT();
void add_style_status_red(lv_obj_t *obj);
void remove_style_status_red(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_STYLES_H*/