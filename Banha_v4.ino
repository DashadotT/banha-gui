/*
  BANHA_v4.ino

  ESP32
  ILI9341 TFT
  XPT2046 Resistive Touch
  SCD41 CO2 + Temperature + Humidity
  INMP441 I2S Microphone
  LVGL 8.x
  EEZ Studio UI

  SENSOR OUTPUT EVERY 5 SECONDS:

  ----------------------
  CO2   : 842 ppm
  Temp  : 29.8 C
  RH    : 72.9 %
  Noise : 51.8 dB
  RMS   : 822.4
  ----------------------

  IMPORTANT:
  - INMP441 does not directly measure calibrated dBA.
  - Noise value is an estimated dB value based on QUIET_RMS.
  - SCD41 normally provides a new measurement approximately
    every 5 seconds.
  - ui_tick() is provided by EEZ Studio's ui.c.
  - DO NOT define another ui_tick() in this file.
  - eez_flow_tick() is intentionally not used.
*/


// =====================================================
// LIBRARIES
// =====================================================

#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <Wire.h>
#include <SensirionI2cScd4x.h>

#include <driver/i2s.h>
#include <math.h>

#include "ui.h"
#include "vars.h"


// =====================================================
// DISPLAY
// =====================================================

static const uint16_t SCREEN_W = 320;
static const uint16_t SCREEN_H = 240;

TFT_eSPI tft = TFT_eSPI();


// =====================================================
// LVGL BUFFER
// =====================================================

static lv_disp_draw_buf_t draw_buf;

static lv_color_t buf1[SCREEN_W * 20];


// =====================================================
// TOUCH
// =====================================================

/*
  WORKING TOUCH CONFIGURATION

  XPT2046 uses HSPI separately from TFT_eSPI.

  Touch wiring:

    T_CLK -> GPIO14
    T_DO  -> GPIO25
    T_DIN -> GPIO26
    T_CS  -> GPIO27
    T_IRQ -> GPIO33

  TFT continues using TFT_eSPI's SPI configuration:

    TFT SCK  -> GPIO18
    TFT MOSI -> GPIO23
    TFT MISO -> GPIO19
    TFT CS   -> GPIO15
*/

#define TOUCH_SCK 14
#define TOUCH_MISO 25
#define TOUCH_MOSI 26
#define TOUCH_CS 27
#define TOUCH_IRQ 33


// Dedicated HSPI bus for XPT2046

SPIClass touchSPI(HSPI);


// XPT2046 with CS + IRQ

XPT2046_Touchscreen ts(
  TOUCH_CS,
  TOUCH_IRQ);


// =====================================================
// TOUCH CALIBRATION
// =====================================================

/*
  THESE VALUES COME FROM YOUR
  CONFIRMED WORKING TOUCH CODE.

  Do not change these unless
  you recalibrate the touchscreen.
*/

static const int TOUCH_RAW_Y_MIN = 3873;
static const int TOUCH_RAW_Y_MAX = 375;

static const int TOUCH_RAW_X_MIN = 258;
static const int TOUCH_RAW_X_MAX = 3783;


// =====================================================
// SCD41
// =====================================================

SensirionI2cScd4x scd4x;

bool scd41Available = false;


// Latest SCD41 values

uint16_t latestCO2 = 0;

float latestTemp = 0.0f;

float latestHumidity = 0.0f;

bool scd41HasData = false;


// =====================================================
// INMP441
// =====================================================

#define I2S_WS 16
#define I2S_SCK 17
#define I2S_SD 5

#define I2S_PORT I2S_NUM_0


// Number of samples for one RMS calculation

#define SAMPLE_BUFFER_SIZE 1024


// =====================================================
// NOISE CALIBRATION
// =====================================================

/*
  QUIET_RMS calibration.

  Current calibration:

      QUIET_RMS = 118.6

  This means:

      118.6 RMS ≈ 35 dB

  Formula:

      dB =
          35 +
          20 * log10(RMS / QUIET_RMS)

  IMPORTANT:

  This is an estimated noise value.

  It is NOT laboratory-calibrated dBA.
*/

const float QUIET_RMS = 118.6f;


// Latest noise values

float latestNoise = 0.0f;

float latestRMS = 0.0f;

bool noiseHasData = false;


// =====================================================
// SENSOR TIMING
// =====================================================

unsigned long lastSensorCycle = 0;


// Read all sensors every 5 seconds

const unsigned long SENSOR_INTERVAL = 5000;


// =====================================================
// DISPLAY FLUSH
// =====================================================

static void disp_flush(
  lv_disp_drv_t *disp,
  const lv_area_t *area,
  lv_color_t *color_p) {

  uint32_t w =
    area->x2 - area->x1 + 1;

  uint32_t h =
    area->y2 - area->y1 + 1;


  tft.startWrite();


  tft.setAddrWindow(
    area->x1,
    area->y1,
    w,
    h);


  tft.pushColors(
    (uint16_t *)&color_p->full,
    w * h,
    true);


  tft.endWrite();


  lv_disp_flush_ready(disp);
}


// =====================================================
// TOUCH READ
// =====================================================

static void touch_read(
  lv_indev_drv_t *indev_drv,
  lv_indev_data_t *data) {

  /*
    No touch detected.

    Tell LVGL that the screen is released.
  */

  if (!ts.touched()) {

    data->state =
      LV_INDEV_STATE_REL;

    return;
  }


  /*
    Touch detected.

    Get raw X/Y values from XPT2046.
  */

  TS_Point p =
    ts.getPoint();


  /*
    WORKING CALIBRATION

    Raw Y -> Screen X
    Raw X -> Screen Y

    This is the exact mapping from
    your confirmed working touch code.
  */

  int x =
    map(
      p.y,
      TOUCH_RAW_Y_MIN,
      TOUCH_RAW_Y_MAX,
      0,
      SCREEN_W - 1);


  int y =
    map(
      p.x,
      TOUCH_RAW_X_MIN,
      TOUCH_RAW_X_MAX,
      0,
      SCREEN_H - 1);


  /*
    Keep coordinates inside
    the LVGL screen.
  */

  x =
    constrain(
      x,
      0,
      SCREEN_W - 1);


  y =
    constrain(
      y,
      0,
      SCREEN_H - 1);


  /*
    Tell LVGL that the screen
    is currently being pressed.
  */

  data->state =
    LV_INDEV_STATE_PR;


  data->point.x =
    x;

  data->point.y =
    y;
}


// =====================================================
// INMP441 INITIALIZATION
// =====================================================

void initINMP441() {

  Serial.println(
    "Initializing INMP441...");


  i2s_config_t i2s_config = {

    .mode =
      (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),

    .sample_rate =
      16000,

    .bits_per_sample =
      I2S_BITS_PER_SAMPLE_32BIT,

    .channel_format =
      I2S_CHANNEL_FMT_ONLY_LEFT,

    .communication_format =
      I2S_COMM_FORMAT_I2S,

    .intr_alloc_flags =
      ESP_INTR_FLAG_LEVEL1,

    .dma_buf_count =
      8,

    .dma_buf_len =
      64,

    .use_apll =
      false,

    .tx_desc_auto_clear =
      false,

    .fixed_mclk =
      0
  };


  i2s_pin_config_t pin_config = {

    .bck_io_num =
      I2S_SCK,

    .ws_io_num =
      I2S_WS,

    .data_out_num =
      I2S_PIN_NO_CHANGE,

    .data_in_num =
      I2S_SD
  };


  esp_err_t result;


  // ---------------------------------------------------
  // Install I2S driver
  // ---------------------------------------------------

  result =
    i2s_driver_install(
      I2S_PORT,
      &i2s_config,
      0,
      NULL);


  if (result != ESP_OK) {

    Serial.print(
      "I2S driver error: ");

    Serial.println(result);

    return;
  }


  // ---------------------------------------------------
  // Configure pins
  // ---------------------------------------------------

  result =
    i2s_set_pin(
      I2S_PORT,
      &pin_config);


  if (result != ESP_OK) {

    Serial.print(
      "I2S pin error: ");

    Serial.println(result);

    return;
  }


  // Clear old DMA data

  i2s_zero_dma_buffer(
    I2S_PORT);


  Serial.println(
    "INMP441 Microphone Ready");
}


// =====================================================
// READ INMP441
// =====================================================

bool readNoiseSensor() {

  /*
    IMPORTANT:

    The sample buffer is STATIC.

    Do NOT change this to:

        int32_t samples[1024];

    inside the function.

    A large local array can consume task stack.

    Keeping it static moves the buffer away from
    the task stack and helps prevent stack overflow.
  */

  static int32_t samples[SAMPLE_BUFFER_SIZE];


  size_t bytesRead = 0;


  esp_err_t result =
    i2s_read(
      I2S_PORT,
      samples,
      sizeof(samples),
      &bytesRead,
      pdMS_TO_TICKS(100));


  if (result != ESP_OK) {

    Serial.print(
      "I2S Read Error: ");

    Serial.println(result);

    return false;
  }


  int sampleCount =
    bytesRead / sizeof(int32_t);


  if (sampleCount <= 0) {

    Serial.println(
      "I2S: No samples");

    return false;
  }


  double sum = 0.0;


  // ===================================================
  // RMS CALCULATION
  // ===================================================

  for (
    int i = 0;
    i < sampleCount;
    i++) {

    float sample =
      samples[i] >> 14;


    sum +=
      sample * sample;
  }


  float rms =
    sqrt(
      sum / sampleCount);


  if (rms < 1.0f) {
    rms = 1.0f;
  }


  // ===================================================
  // ESTIMATED dB
  // ===================================================

  float dB =
    35.0f + 20.0f * log10(rms / QUIET_RMS);


  // Limit value

  if (dB < 0.0f) {
    dB = 0.0f;
  }


  if (dB > 120.0f) {
    dB = 120.0f;
  }


  // ===================================================
  // SAVE VALUES
  // ===================================================

  latestRMS =
    rms;


  latestNoise =
    dB;


  noiseHasData =
    true;


  // ===================================================
  // UPDATE EEZ NOISE VALUE
  // ===================================================

  set_var_noise_value(
    latestNoise);


  // ===================================================
  // NOISE STATUS
  // ===================================================

  if (latestNoise <= 35.0f) {

    set_var_noise_status(
      "NORMAL");

  } else if (
    latestNoise <= 55.0f) {

    set_var_noise_status(
      "MODERATE");

  } else {

    set_var_noise_status(
      "POOR");
  }


  return true;
}


// =====================================================
// READ SCD41
// =====================================================

bool readSCD41() {

  if (!scd41Available) {
    return false;
  }


  bool dataReady =
    false;


  uint16_t error =
    scd4x.getDataReadyStatus(
      dataReady);


  if (error) {

    Serial.print(
      "SCD41 Data Ready Error: ");

    Serial.println(error);

    return false;
  }


  if (!dataReady) {
    return false;
  }


  uint16_t co2;

  float temp;

  float humidity;


  error =
    scd4x.readMeasurement(
      co2,
      temp,
      humidity);


  if (error) {

    Serial.print(
      "SCD41 Read Error: ");

    Serial.println(error);

    return false;
  }


  // ===================================================
  // SAVE VALUES
  // ===================================================

  latestCO2 =
    co2;


  latestTemp =
    roundf(
      temp * 10.0f)
    / 10.0f;


  latestHumidity =
    humidity;


  scd41HasData =
    true;


  // ===================================================
  // UPDATE EEZ UI
  // ===================================================

  set_var_co2_value(
    latestCO2);


  set_var_temp_value(
    latestTemp);


  // ===================================================
  // TEMPERATURE STATUS
  // ===================================================

  if (latestTemp < 22.0f) {

    set_var_temp_status(
      "POOR");

  } else if (
    latestTemp < 23.0f) {

    set_var_temp_status(
      "MODERATE");

  } else if (
    latestTemp <= 26.0f) {

    set_var_temp_status(
      "NORMAL");

  } else if (
    latestTemp <= 27.1f) {

    set_var_temp_status(
      "MODERATE");

  } else {

    set_var_temp_status(
      "POOR");
  }


  // ===================================================
  // CO2 STATUS
  // ===================================================

  if (latestCO2 <= 1000) {

    set_var_co2_status(
      "NORMAL");

  } else if (
    latestCO2 <= 1500) {

    set_var_co2_status(
      "MODERATE");

  } else {

    set_var_co2_status(
      "POOR");
  }


  return true;
}


// =====================================================
// PRINT ALL SENSOR VALUES TOGETHER
// =====================================================

void printSensorData() {

  Serial.println(
    "----------------------");


  // ===================================================
  // CO2
  // ===================================================

  Serial.print(
    "CO2   : ");


  if (scd41HasData) {

    Serial.print(
      latestCO2);

    Serial.println(
      " ppm");

  } else {

    Serial.println(
      "-- ppm");
  }


  // ===================================================
  // TEMPERATURE
  // ===================================================

  Serial.print(
    "Temp  : ");


  if (scd41HasData) {

    Serial.print(
      latestTemp,
      1);

    Serial.println(
      " C");

  } else {

    Serial.println(
      "-- C");
  }


  // ===================================================
  // HUMIDITY
  // ===================================================

  Serial.print(
    "RH    : ");


  if (scd41HasData) {

    Serial.print(
      latestHumidity,
      1);

    Serial.println(
      " %");

  } else {

    Serial.println(
      "-- %");
  }


  // ===================================================
  // NOISE
  // ===================================================

  Serial.print(
    "Noise : ");


  if (noiseHasData) {

    Serial.print(
      latestNoise,
      1);

    Serial.println(
      " dB");

  } else {

    Serial.println(
      "-- dB");
  }


  // ===================================================
  // RMS
  // ===================================================

  Serial.print(
    "RMS   : ");


  if (noiseHasData) {

    Serial.println(
      latestRMS,
      1);

  } else {

    Serial.println(
      "--");
  }


  Serial.println(
    "----------------------");
}


// =====================================================
// INITIALIZE SCD41
// =====================================================

void initSCD41() {

  Serial.println(
    "Starting SCD41...");


  /*
    Wiring:

    SCD41 SDA -> GPIO22
    SCD41 SCL -> GPIO21
  */

  Wire.begin(
    22,
    21);


  scd4x.begin(
    Wire,
    0x62);


  uint16_t error;


  // ===================================================
  // STOP PREVIOUS MEASUREMENT
  // ===================================================

  Serial.println(
    "Stopping previous measurement...");


  error =
    scd4x.stopPeriodicMeasurement();


  if (error) {

    Serial.print(
      "Stop measurement error: ");

    Serial.println(error);


    /*
      Error 270 can occur during initialization
      if the SCD41 is not currently running or
      is temporarily unavailable.

      We do not continuously retry here.
    */

    delay(500);

  } else {

    delay(500);
  }


  // ===================================================
  // START PERIODIC MEASUREMENT
  // ===================================================

  Serial.println(
    "Starting SCD41 periodic measurement...");


  error =
    scd4x.startPeriodicMeasurement();


  if (error) {

    Serial.print(
      "SCD41 Error: ");

    Serial.println(error);


    Serial.println(
      "SCD41 failed to start.");


    Serial.println(
      "Continuing without SCD41.");


    scd41Available =
      false;

  } else {

    Serial.println(
      "SCD41 Ready");


    scd41Available =
      true;
  }
}


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(
    115200);


  delay(1000);


  Serial.println();


  Serial.println(
    "==================================");


  Serial.println(
    "       BANHA SENSOR TEST");


  Serial.println(
    "==================================");


  // ===================================================
  // SCD41
  // ===================================================

  initSCD41();


  // ===================================================
  // INMP441
  // ===================================================

  initINMP441();


  // ===================================================
  // TFT
  // ===================================================

  Serial.println(
    "Starting TFT...");


  tft.begin();


  tft.setRotation(
    1);


  tft.fillScreen(
    TFT_BLACK);


  // ===================================================
  // TOUCH
  // ===================================================

  Serial.println(
    "Starting Touch...");


  /*
    IMPORTANT:

    The working touch code uses a dedicated HSPI bus.

    This prevents the XPT2046 from interfering with
    the TFT_eSPI SPI configuration.
  */

  touchSPI.begin(
    TOUCH_SCK,
    TOUCH_MISO,
    TOUCH_MOSI,
    TOUCH_CS);


  ts.begin(
    touchSPI);


  /*
    Keep XPT2046 rotation at 0.

    The raw-coordinate mapping below already handles
    the physical orientation of the display.
  */

  ts.setRotation(0);


  Serial.println(
    "Touch Ready");


  // ===================================================
  // LVGL
  // ===================================================

  Serial.println(
    "Starting LVGL...");


  lv_init();


  lv_disp_draw_buf_init(
    &draw_buf,
    buf1,
    NULL,
    SCREEN_W * 20);


  // ===================================================
  // LVGL DISPLAY DRIVER
  // ===================================================

  static lv_disp_drv_t disp_drv;


  lv_disp_drv_init(
    &disp_drv);


  disp_drv.hor_res =
    SCREEN_W;


  disp_drv.ver_res =
    SCREEN_H;


  disp_drv.flush_cb =
    disp_flush;


  disp_drv.draw_buf =
    &draw_buf;


  lv_disp_drv_register(
    &disp_drv);


  // ===================================================
  // LVGL TOUCH DRIVER
  // ===================================================

  static lv_indev_drv_t indev_drv;


  lv_indev_drv_init(
    &indev_drv);


  indev_drv.type =
    LV_INDEV_TYPE_POINTER;


  indev_drv.read_cb =
    touch_read;


  lv_indev_drv_register(
    &indev_drv);


  // ===================================================
  // EEZ STUDIO UI
  // ===================================================

  Serial.println(
    "Starting EEZ UI...");


  ui_init();


  // ===================================================
  // INITIAL SENSOR TIMER
  // ===================================================

  lastSensorCycle =
    millis();


  // ===================================================
  // READY
  // ===================================================

  Serial.println();


  Serial.println(
    "==================================");


  Serial.println(
    "        BANHA SYSTEM READY");


  Serial.println(
    "==================================");


  Serial.println();


  Serial.println(
    "Sensor interval: 5 seconds");


  Serial.println(
    "Noise RMS buffer: 1024 samples");


  Serial.println(
    "INMP441 sample rate: 16000 Hz");


  Serial.println(
    "SCD41: 5-second periodic measurement");


  Serial.println(
    "XPT2046: HSPI");


  Serial.println(
    "Touch pins: CLK=14 MISO=25 MOSI=26 CS=27 IRQ=33");


  Serial.println(
    "EEZ flow tick: DISABLED");


  Serial.println(
    "ui_tick(): PROVIDED BY ui.c");


  Serial.println();
}


// =====================================================
// LOOP
// =====================================================

void loop() {

  // ===================================================
  // LVGL
  // ===================================================

  /*
    Use actual elapsed time instead of assuming
    loop() always takes exactly 5 ms.
  */

  static uint32_t lastTick =
    millis();


  uint32_t now =
    millis();


  lv_tick_inc(
    now - lastTick);


  lastTick =
    now;


  lv_timer_handler();


  /*
    IMPORTANT:

    ui_tick() is NOT defined here.

    It comes from your generated ui.c.

    This prevents:

      multiple definition of 'ui_tick'
  */

  ui_tick();


  // ===================================================
  // SENSOR CYCLE
  // ===================================================

  if (
    millis() - lastSensorCycle >= SENSOR_INTERVAL) {

    lastSensorCycle =
      millis();


    // -------------------------------------------------
    // Read SCD41
    // -------------------------------------------------

    readSCD41();


    // -------------------------------------------------
    // Read INMP441
    // -------------------------------------------------

    readNoiseSensor();


    // -------------------------------------------------
    // Print ALL sensors together
    // -------------------------------------------------

    printSensorData();
  }


  // ===================================================
  // SMALL DELAY
  // ===================================================

  delay(1);
}