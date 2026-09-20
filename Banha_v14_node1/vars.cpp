#include <string.h>
#include "vars.h"

//=====================================================
// Temperature
//=====================================================

static float temp_value = 0.0f;
static char temp_status[20] = "--";

float get_var_temp_value() {
  return temp_value;
}

void set_var_temp_value(float value) {
  temp_value = value;
}

const char *get_var_temp_status() {
  return temp_status;
}

void set_var_temp_status(const char *value) {
  if (value == NULL) {
    return;
  }

  strncpy(temp_status, value, sizeof(temp_status) - 1);
  temp_status[sizeof(temp_status) - 1] = '\0';
}


//=====================================================
// Noise
//=====================================================

static int32_t noise_value = 0;
static char noise_status[20] = "--";

int32_t get_var_noise_value() {
  return noise_value;
}

void set_var_noise_value(int32_t value) {
  noise_value = value;
}

const char *get_var_noise_status() {
  return noise_status;
}

void set_var_noise_status(const char *value) {
  if (value == NULL) {
    return;
  }

  strncpy(noise_status, value, sizeof(noise_status) - 1);
  noise_status[sizeof(noise_status) - 1] = '\0';
}


//=====================================================
// Nodes Connection Status
//=====================================================

static char nodes_status[20] = "DISCONNECTED";

const char *get_var_nodes_status() {
  return nodes_status;
}

void set_var_nodes_status(const char *value) {
  if (value == NULL) {
    return;
  }

  strncpy(nodes_status, value, sizeof(nodes_status) - 1);
  nodes_status[sizeof(nodes_status) - 1] = '\0';
}


//=====================================================
// Packet Status
//=====================================================

static int32_t packet_status = 0;

int32_t get_var_packet_status() {
  return packet_status;
}

void set_var_packet_status(int32_t value) {
  packet_status = value;
}