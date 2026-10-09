#include "styles.h"
#include "images.h"
#include "fonts.h"

#include "ui.h"
#include "screens.h"

//
// Style: status_orange
//

void init_style_status_orange_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_bg_color(style, lv_color_hex(0xebaf1c));
    lv_style_set_text_color(style, lv_color_hex(0xebaf1c));
};

lv_style_t *get_style_status_orange_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_mem_alloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_status_orange_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_status_orange(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_status_orange_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_status_orange(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_status_orange_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
// Style: status_green
//

void init_style_status_green_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_text_color(style, lv_color_hex(0x00bf63));
};

lv_style_t *get_style_status_green_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_mem_alloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_status_green_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_status_green(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_status_green_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_status_green(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_status_green_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
// Style: status_red
//

void init_style_status_red_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_bg_color(style, lv_color_hex(0xff383c));
    lv_style_set_text_color(style, lv_color_hex(0xff383c));
};

lv_style_t *get_style_status_red_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_mem_alloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_status_red_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_status_red(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_status_red_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_status_red(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_status_red_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
// Style: nodes_status_green
//

void init_style_nodes_status_green_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_text_color(style, lv_color_hex(0x00bf63));
};

lv_style_t *get_style_nodes_status_green_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_mem_alloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_nodes_status_green_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_nodes_status_green(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_nodes_status_green_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_nodes_status_green(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_nodes_status_green_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
// Style: nodes_status_red
//

void init_style_nodes_status_red_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_text_color(style, lv_color_hex(0xff383c));
};

lv_style_t *get_style_nodes_status_red_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_mem_alloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_nodes_status_red_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_nodes_status_red(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_nodes_status_red_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_nodes_status_red(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_nodes_status_red_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
// Style: packet_status_gray
//

void init_style_packet_status_gray_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_text_color(style, lv_color_hex(0x858585));
};

lv_style_t *get_style_packet_status_gray_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_mem_alloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_packet_status_gray_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_packet_status_gray(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_packet_status_gray_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_packet_status_gray(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_packet_status_gray_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
// Style: packet_status_white
//

void init_style_packet_status_white_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_text_color(style, lv_color_hex(0xffffff));
};

lv_style_t *get_style_packet_status_white_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_mem_alloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_packet_status_white_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_packet_status_white(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_packet_status_white_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_packet_status_white(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_packet_status_white_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
//
//

void add_style(lv_obj_t *obj, int32_t styleIndex) {
    typedef void (*AddStyleFunc)(lv_obj_t *obj);
    static const AddStyleFunc add_style_funcs[] = {
        add_style_status_orange,
        add_style_status_green,
        add_style_status_red,
        add_style_nodes_status_green,
        add_style_nodes_status_red,
        add_style_packet_status_gray,
        add_style_packet_status_white,
    };
    add_style_funcs[styleIndex](obj);
}

void remove_style(lv_obj_t *obj, int32_t styleIndex) {
    typedef void (*RemoveStyleFunc)(lv_obj_t *obj);
    static const RemoveStyleFunc remove_style_funcs[] = {
        remove_style_status_orange,
        remove_style_status_green,
        remove_style_status_red,
        remove_style_nodes_status_green,
        remove_style_nodes_status_red,
        remove_style_packet_status_gray,
        remove_style_packet_status_white,
    };
    remove_style_funcs[styleIndex](obj);
}