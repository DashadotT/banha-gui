#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_MAIN_SCREEN = 1,
    SCREEN_ID_SELECTION_SCREEN = 2,
    SCREEN_ID_RECORDING_SCREEN = 3,
    _SCREEN_ID_LAST = 3
};

typedef struct _objects_t {
    lv_obj_t *main_screen;
    lv_obj_t *selection_screen;
    lv_obj_t *recording_screen;
    lv_obj_t *header;
    lv_obj_t *obj0;
    lv_obj_t *obj1;
    lv_obj_t *obj2;
    lv_obj_t *obj3;
    lv_obj_t *temp_container;
    lv_obj_t *obj4;
    lv_obj_t *obj5;
    lv_obj_t *temp_label;
    lv_obj_t *obj6;
    lv_obj_t *obj7;
    lv_obj_t *obj8;
    lv_obj_t *temp_status_label;
    lv_obj_t *obj9;
    lv_obj_t *obj10;
    lv_obj_t *co2_container;
    lv_obj_t *obj11;
    lv_obj_t *obj12;
    lv_obj_t *co2_label;
    lv_obj_t *obj13;
    lv_obj_t *obj14;
    lv_obj_t *obj15;
    lv_obj_t *co2_status_label;
    lv_obj_t *obj16;
    lv_obj_t *obj17;
    lv_obj_t *noise_container;
    lv_obj_t *obj18;
    lv_obj_t *obj19;
    lv_obj_t *noise_label;
    lv_obj_t *obj20;
    lv_obj_t *obj21;
    lv_obj_t *obj22;
    lv_obj_t *noise_status_label;
    lv_obj_t *obj23;
    lv_obj_t *obj24;
    lv_obj_t *obj25;
    lv_obj_t *obj26;
    lv_obj_t *obj27;
    lv_obj_t *obj28;
    lv_obj_t *obj29;
    lv_obj_t *obj30;
    lv_obj_t *obj31;
    lv_obj_t *obj32;
    lv_obj_t *obj33;
    lv_obj_t *obj34;
    lv_obj_t *obj35;
    lv_obj_t *obj36;
    lv_obj_t *obj37;
    lv_obj_t *header_2;
    lv_obj_t *obj38;
    lv_obj_t *obj39;
    lv_obj_t *obj40;
    lv_obj_t *obj41;
    lv_obj_t *obj42;
    lv_obj_t *record_icon;
    lv_obj_t *temp_container_1;
    lv_obj_t *obj43;
    lv_obj_t *obj44;
    lv_obj_t *temp_label_1;
    lv_obj_t *obj45;
    lv_obj_t *obj46;
    lv_obj_t *obj47;
    lv_obj_t *rec_temp_status;
    lv_obj_t *obj48;
    lv_obj_t *obj49;
    lv_obj_t *co2_container_1;
    lv_obj_t *obj50;
    lv_obj_t *obj51;
    lv_obj_t *co2_label_1;
    lv_obj_t *obj52;
    lv_obj_t *obj53;
    lv_obj_t *obj54;
    lv_obj_t *rec_co2_status;
    lv_obj_t *obj55;
    lv_obj_t *obj56;
    lv_obj_t *noise_container_1;
    lv_obj_t *obj57;
    lv_obj_t *obj58;
    lv_obj_t *noise_label_1;
    lv_obj_t *obj59;
    lv_obj_t *obj60;
    lv_obj_t *obj61;
    lv_obj_t *rec_noise_status;
    lv_obj_t *obj62;
    lv_obj_t *obj63;
} objects_t;

extern objects_t objects;

void create_screen_main_screen();
void tick_screen_main_screen();

void create_screen_selection_screen();
void tick_screen_selection_screen();

void create_screen_recording_screen();
void tick_screen_recording_screen();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/