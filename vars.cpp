#include "vars.h"
#include <string.h>

static float temp_value = 0.0f;
static char temp_status[32] = "Normal";

static int32_t co2_value = 0;
static char co2_status[32] = "Normal";

static int32_t noise_value = 0;
static char noise_status[32] = "Normal";

extern "C"
{

    // Temperature
    float get_var_temp_value()
    {
        return temp_value;
    }

    void set_var_temp_value(float value)
    {
        temp_value = value;
    }

    const char *get_var_temp_status()
    {
        return temp_status;
    }

    void set_var_temp_status(const char *value)
    {
        strncpy(temp_status, value, sizeof(temp_status) - 1);
        temp_status[sizeof(temp_status) - 1] = '\0';
    }

    // CO₂
    int32_t get_var_co2_value()
    {
        return co2_value;
    }

    void set_var_co2_value(int32_t value)
    {
        co2_value = value;
    }

    const char *get_var_co2_status()
    {
        return co2_status;
    }

    void set_var_co2_status(const char *value)
    {
        strncpy(co2_status, value, sizeof(co2_status) - 1);
        co2_status[sizeof(co2_status) - 1] = '\0';
    }

    // Noise
    int32_t get_var_noise_value()
    {
        return noise_value;
    }

    void set_var_noise_value(int32_t value)
    {
        noise_value = value;
    }

    const char *get_var_noise_status()
    {
        return noise_status;
    }

    void set_var_noise_status(const char *value)
    {
        strncpy(noise_status, value, sizeof(noise_status) - 1);
        noise_status[sizeof(noise_status) - 1] = '\0';
    }
}