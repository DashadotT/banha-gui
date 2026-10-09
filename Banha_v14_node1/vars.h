#ifndef EEZ_LVGL_UI_VARS_H
#define EEZ_LVGL_UI_VARS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// enum declarations

// Flow global variables

enum FlowGlobalVariables {
    FLOW_GLOBAL_VARIABLE_NONE
};

// Native global variables

extern float get_var_temp_value();
extern void set_var_temp_value(float value);
extern const char *get_var_temp_status();
extern void set_var_temp_status(const char *value);
extern int32_t get_var_noise_value();
extern void set_var_noise_value(int32_t value);
extern const char *get_var_noise_status();
extern void set_var_noise_status(const char *value);
extern const char *get_var_nodes_status();
extern void set_var_nodes_status(const char *value);
extern int32_t get_var_packet_status();
extern void set_var_packet_status(int32_t value);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_VARS_H*/