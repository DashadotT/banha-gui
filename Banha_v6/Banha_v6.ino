/*
  =====================================================
  BANHA_v4.ino
  NODE 1 - SENSOR + DISPLAY + 1-MINUTE LORA AVERAGE
  =====================================================

  ESP32
  ILI9341 TFT
  XPT2046 Resistive Touch
  SCD41 CO2 + Temperature + Humidity
  INMP441 I2S Microphone
  RA-02 SX1278 LoRa
  LVGL 8.x
  EEZ Studio UI

  =====================================================
  DATA COLLECTION
  =====================================================

  Sensors are read every 5 seconds.

  12 valid readings are collected:

      12 x 5 seconds = approximately 60 seconds

  After 12 valid readings:

      1. Calculate average CO2
      2. Calculate average Temperature
      3. Calculate average Noise
      4. Send average values through LoRa
      5. Clear temporary RAM
      6. Start next collection

  =====================================================
  LORA PACKET FORMAT
  =====================================================

  NODE:1,PACKET:1,AVG_CO2:842.5,AVG_TEMP:29.8,AVG_NOISE:51.8

  =====================================================
  RA-02 SX1278 WIRING
  =====================================================

  VCC   -> 3.3V
  GND   -> GND

  SCK   -> GPIO18   Shared with TFT
  MISO  -> GPIO19   Shared with TFT
  MOSI  -> GPIO23   Shared with TFT

  NSS   -> GPIO13
  RESET -> GPIO32
  DIO0  -> GPIO34

  =====================================================
  TFT WIRING
  =====================================================

  TFT CS   -> GPIO15
  TFT DC   -> GPIO2
  TFT RST  -> GPIO4

  TFT MOSI -> GPIO23
  TFT MISO -> GPIO19
  TFT SCK  -> GPIO18

  =====================================================
  TOUCH WIRING
  =====================================================

  T_CLK -> GPIO14
  T_DO  -> GPIO25
  T_DIN -> GPIO26
  T_CS  -> GPIO27
  T_IRQ -> GPIO33

  =====================================================
  SCD41 WIRING
  =====================================================

  SDA -> GPIO22
  SCL -> GPIO21

  =====================================================
  INMP441 WIRING
  =====================================================

  WS  -> GPIO16
  SCK -> GPIO17
  SD  -> GPIO5

  L/R -> GND

  =====================================================
  IMPORTANT
  =====================================================

  - TFT and LoRa share VSPI pins.
  - TFT CS = GPIO15
  - LoRa NSS = GPIO13
  - Touch uses HSPI.
  - INMP441 noise is estimated dB, NOT calibrated laboratory dBA.
  - ui_tick() is provided by EEZ Studio ui.c.
*/


// =====================================================
// LIBRARIES
// =====================================================

#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#include <SPI.h>
#include <LoRa.h>

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
// LORA
// =====================================================

#define LORA_SCK 18
#define LORA_MISO 19
#define LORA_MOSI 23

#define LORA_SS 13
#define LORA_RST 32
#define LORA_DIO0 34

#define LORA_BAND 433E6


// =====================================================
// LORA STATUS
// =====================================================

bool loraAvailable = false;

unsigned long loraPacketNumber = 0;


// =====================================================
// TOUCH
// =====================================================

#define TOUCH_SCK 14
#define TOUCH_MISO 25
#define TOUCH_MOSI 26
#define TOUCH_CS 27
#define TOUCH_IRQ 33

SPIClass touchSPI(HSPI);

XPT2046_Touchscreen ts(
  TOUCH_CS,
  TOUCH_IRQ);


// =====================================================
// TOUCH CALIBRATION
// =====================================================

static const int TOUCH_RAW_Y_MIN = 3873;
static const int TOUCH_RAW_Y_MAX = 375;

static const int TOUCH_RAW_X_MIN = 258;
static const int TOUCH_RAW_X_MAX = 3783;


// =====================================================
// SCD41
// =====================================================

SensirionI2cScd4x scd4x;

bool scd41Available = false;


// Latest sensor values

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

#define SAMPLE_BUFFER_SIZE 1024


// =====================================================
// NOISE CALIBRATION
// =====================================================

/*
  Current calibration:

      QUIET_RMS = 118.6
      118.6 RMS ≈ 35 dB

  Formula:

      dB = 35 + 20 * log10(RMS / QUIET_RMS)

  NOTE:
  This is an estimated noise level.
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

const unsigned long SENSOR_INTERVAL = 5000;


// =====================================================
// 1-MINUTE DATA STORAGE
// =====================================================

/*
  12 valid readings x 5 seconds
  = approximately 60 seconds
*/

#define READINGS_PER_MINUTE 12

float co2Readings[READINGS_PER_MINUTE];

float tempReadings[READINGS_PER_MINUTE];

float noiseReadings[READINGS_PER_MINUTE];

int storedReadingCount = 0;


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

  if (!ts.touched()) {

    data->state =
      LV_INDEV_STATE_REL;

    return;
  }


  TS_Point p =
    ts.getPoint();


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


  data->state =
    LV_INDEV_STATE_PR;


  data->point.x =
    x;


  data->point.y =
    y;
}


// =====================================================
// INITIALIZE LORA
// =====================================================

void initLoRa() {

  Serial.println(
    "Starting LoRa...");


  /*
    LoRa uses VSPI.

    The TFT also uses these SPI pins.

    Device selection is handled through
    separate CS pins:

      TFT  -> GPIO15
      LoRa -> GPIO13
  */

  SPI.begin(
    LORA_SCK,
    LORA_MISO,
    LORA_MOSI,
    LORA_SS);


  LoRa.setPins(
    LORA_SS,
    LORA_RST,
    LORA_DIO0);


  if (!LoRa.begin(LORA_BAND)) {

    Serial.println(
      "LoRa initialization FAILED!");

    Serial.println(
      "Continuing without LoRa.");

    loraAvailable = false;

    return;
  }


  Serial.println(
    "LoRa Ready!");

  Serial.println(
    "Node 1 will send 1-minute averages.");


  loraAvailable = true;
}


// =====================================================
// CLEAR TEMPORARY DATA
// =====================================================

void clearStoredData() {

  Serial.println(
    "Clearing temporary 1-minute RAM data...");


  for (
    int i = 0;
    i < READINGS_PER_MINUTE;
    i++) {

    co2Readings[i] = 0.0f;

    tempReadings[i] = 0.0f;

    noiseReadings[i] = 0.0f;
  }


  storedReadingCount = 0;


  Serial.println(
    "RAM data cleared.");
}


// =====================================================
// STORE CURRENT SENSOR DATA
// =====================================================

void storeSensorData() {

  if (
    storedReadingCount >= READINGS_PER_MINUTE) {

    Serial.println(
      "Storage full.");

    return;
  }


  // ---------------------------------------------------
  // REQUIRE VALID SCD41 DATA
  // ---------------------------------------------------

  if (!scd41HasData) {

    Serial.println(
      "SCD41 data unavailable. Reading not stored.");

    return;
  }


  // ---------------------------------------------------
  // REQUIRE VALID NOISE DATA
  // ---------------------------------------------------

  if (!noiseHasData) {

    Serial.println(
      "Noise data unavailable. Reading not stored.");

    return;
  }


  // ---------------------------------------------------
  // STORE VALUES
  // ---------------------------------------------------

  co2Readings[storedReadingCount] =
    (float)latestCO2;


  tempReadings[storedReadingCount] =
    latestTemp;


  noiseReadings[storedReadingCount] =
    latestNoise;


  storedReadingCount++;


  // ---------------------------------------------------
  // PRINT STORAGE STATUS
  // ---------------------------------------------------

  Serial.println(
    "================================");


  Serial.print(
    "Stored reading: ");

  Serial.print(
    storedReadingCount);

  Serial.print(
    " / ");

  Serial.println(
    READINGS_PER_MINUTE);


  Serial.print(
    "Stored CO2: ");

  Serial.print(
    latestCO2);

  Serial.println(
    " ppm");


  Serial.print(
    "Stored Temp: ");

  Serial.print(
    latestTemp,
    1);

  Serial.println(
    " C");


  Serial.print(
    "Stored Noise: ");

  Serial.print(
    latestNoise,
    1);

  Serial.println(
    " dB");


  Serial.println(
    "================================");
}


// =====================================================
// CALCULATE AND SEND 1-MINUTE AVERAGE
// =====================================================

void calculateAndSendAverage() {

  if (
    storedReadingCount < READINGS_PER_MINUTE) {

    return;
  }


  Serial.println();

  Serial.println(
    "================================");

  Serial.println(
    "CALCULATING 1-MINUTE AVERAGES");

  Serial.println(
    "================================");


  float totalCO2 = 0.0f;

  float totalTemp = 0.0f;

  float totalNoise = 0.0f;


  // ---------------------------------------------------
  // ADD ALL 12 READINGS
  // ---------------------------------------------------

  for (
    int i = 0;
    i < READINGS_PER_MINUTE;
    i++) {

    totalCO2 +=
      co2Readings[i];


    totalTemp +=
      tempReadings[i];


    totalNoise +=
      noiseReadings[i];
  }


  // ---------------------------------------------------
  // CALCULATE AVERAGES
  // ---------------------------------------------------

  float averageCO2 =
    totalCO2 / READINGS_PER_MINUTE;


  float averageTemp =
    totalTemp / READINGS_PER_MINUTE;


  float averageNoise =
    totalNoise / READINGS_PER_MINUTE;


  // ---------------------------------------------------
  // PRINT AVERAGES
  // ---------------------------------------------------

  Serial.print(
    "Average CO2: ");

  Serial.print(
    averageCO2,
    1);

  Serial.println(
    " ppm");


  Serial.print(
    "Average Temp: ");

  Serial.print(
    averageTemp,
    1);

  Serial.println(
    " C");


  Serial.print(
    "Average Noise: ");

  Serial.print(
    averageNoise,
    1);

  Serial.println(
    " dB");


  // ---------------------------------------------------
  // CHECK LORA
  // ---------------------------------------------------

  if (!loraAvailable) {

    Serial.println(
      "LoRa unavailable.");

    Serial.println(
      "Average not sent.");


    clearStoredData();

    return;
  }


  // ---------------------------------------------------
  // INCREASE PACKET NUMBER
  // ---------------------------------------------------

  loraPacketNumber++;


  // ---------------------------------------------------
  // CREATE LORA PACKET
  // ---------------------------------------------------

  String message;

  message.reserve(100);


  message += "NODE:1";

  message += ",PACKET:";

  message +=
    String(loraPacketNumber);

  message += ",AVG_CO2:";

  message +=
    String(averageCO2, 1);

  message += ",AVG_TEMP:";

  message +=
    String(averageTemp, 1);

  message += ",AVG_NOISE:";

  message +=
    String(averageNoise, 1);


  // ---------------------------------------------------
  // PRINT PACKET
  // ---------------------------------------------------

  Serial.println();

  Serial.println(
    "Sending 1-minute average to Node 2...");


  Serial.println(
    "LoRa Packet:");


  Serial.println(
    message);


  // ---------------------------------------------------
  // SEND THROUGH LORA
  // ---------------------------------------------------

  LoRa.beginPacket();

  LoRa.print(
    message);

  int result =
    LoRa.endPacket();


  // ---------------------------------------------------
  // SEND RESULT
  // ---------------------------------------------------

  if (result == 1) {

    Serial.println(
      "1-minute average sent successfully!");

  } else {

    Serial.println(
      "LoRa sending FAILED!");
  }


  // ---------------------------------------------------
  // CLEAR RAM
  // ---------------------------------------------------

  clearStoredData();


  Serial.println(
    "================================");

  Serial.println(
    "Starting next 1-minute collection...");

  Serial.println(
    "================================");
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
  // INSTALL I2S DRIVER
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

    Serial.println(
      result);

    return;
  }


  // ---------------------------------------------------
  // SET I2S PINS
  // ---------------------------------------------------

  result =
    i2s_set_pin(
      I2S_PORT,
      &pin_config);


  if (result != ESP_OK) {

    Serial.print(
      "I2S pin error: ");

    Serial.println(
      result);

    return;
  }


  // ---------------------------------------------------
  // CLEAR DMA BUFFER
  // ---------------------------------------------------

  i2s_zero_dma_buffer(
    I2S_PORT);


  Serial.println(
    "INMP441 Microphone Ready");
}


// =====================================================
// READ INMP441
// =====================================================

bool readNoiseSensor() {

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

    Serial.println(
      result);

    return false;
  }


  int sampleCount =
    bytesRead / sizeof(int32_t);


  if (sampleCount <= 0) {

    Serial.println(
      "I2S: No samples");

    return false;
  }


  // ---------------------------------------------------
  // CALCULATE RMS
  // ---------------------------------------------------

  double sum = 0.0;


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


  // ---------------------------------------------------
  // CONVERT RMS TO ESTIMATED dB
  // ---------------------------------------------------

  float dB =
    35.0f + 20.0f * log10(rms / QUIET_RMS);


  // ---------------------------------------------------
  // LIMIT RESULT
  // ---------------------------------------------------

  if (dB < 0.0f) {

    dB = 0.0f;
  }


  if (dB > 120.0f) {

    dB = 120.0f;
  }


  latestRMS =
    rms;


  latestNoise =
    dB;


  noiseHasData =
    true;


  // ---------------------------------------------------
  // UPDATE EEZ NOISE VALUE
  // ---------------------------------------------------

  set_var_noise_value(
    latestNoise);


  // ---------------------------------------------------
  // UPDATE NOISE STATUS
  // ---------------------------------------------------

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

    Serial.println(
      error);

    return false;
  }


  if (!dataReady) {

    Serial.println(
      "SCD41: New data not ready yet.");

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

    Serial.println(
      error);

    return false;
  }


  // ---------------------------------------------------
  // SAVE VALUES
  // ---------------------------------------------------

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


  // ---------------------------------------------------
  // UPDATE EEZ CO2
  // ---------------------------------------------------

  set_var_co2_value(
    latestCO2);


  // ---------------------------------------------------
  // UPDATE EEZ TEMPERATURE
  // ---------------------------------------------------

  set_var_temp_value(
    latestTemp);


  // ---------------------------------------------------
  // TEMPERATURE STATUS
  // ---------------------------------------------------

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


  // ---------------------------------------------------
  // CO2 STATUS
  // ---------------------------------------------------

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
// PRINT SENSOR DATA
// =====================================================

void printSensorData() {

  Serial.println(
    "----------------------");


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


  Serial.print(
    "1-Min Storage: ");

  Serial.print(
    storedReadingCount);

  Serial.print(
    " / ");

  Serial.println(
    READINGS_PER_MINUTE);


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
    SCD41 wiring:

    SDA -> GPIO22
    SCL -> GPIO21
  */

  Wire.begin(
    22,
    21);


  scd4x.begin(
    Wire,
    0x62);


  uint16_t error;


  // ---------------------------------------------------
  // STOP PREVIOUS MEASUREMENT
  // ---------------------------------------------------

  Serial.println(
    "Stopping previous measurement...");


  error =
    scd4x.stopPeriodicMeasurement();


  if (error) {

    Serial.print(
      "Stop measurement error: ");

    Serial.println(
      error);
  }


  delay(500);


  // ---------------------------------------------------
  // START PERIODIC MEASUREMENT
  // ---------------------------------------------------

  Serial.println(
    "Starting SCD41 periodic measurement...");


  error =
    scd4x.startPeriodicMeasurement();


  if (error) {

    Serial.print(
      "SCD41 Error: ");

    Serial.println(
      error);


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
    " BANHA NODE 1 SENSOR SYSTEM");

  Serial.println(
    "==================================");


  // ===================================================
  // CLEAR TEMPORARY RAM
  // ===================================================

  clearStoredData();


  // ===================================================
  // SCD41
  // ===================================================

  initSCD41();


  // ===================================================
  // INMP441
  // ===================================================

  initINMP441();

  // ===================================================
  // LORA
  // ===================================================

  initLoRa();

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


  touchSPI.begin(
    TOUCH_SCK,
    TOUCH_MISO,
    TOUCH_MOSI,
    TOUCH_CS);


  ts.begin(
    touchSPI);


  ts.setRotation(
    0);


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
  // DISPLAY DRIVER
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
  // TOUCH DRIVER
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
  // EEZ UI
  // ===================================================

  Serial.println(
    "Starting EEZ UI...");


  ui_init();


  // ===================================================
  // SENSOR TIMER
  // ===================================================

  lastSensorCycle =
    millis();


  // ===================================================
  // SYSTEM READY
  // ===================================================

  Serial.println();


  Serial.println(
    "==================================");

  Serial.println(
    " BANHA SYSTEM READY");

  Serial.println(
    "==================================");


  Serial.println();

  Serial.println(
    "Sensor interval: 5 seconds");

  Serial.println(
    "Readings per transmission: 12");

  Serial.println(
    "Transmission interval: ~60 seconds");

  Serial.println(
    "LoRa sends: AVG_CO2, AVG_TEMP, AVG_NOISE");

  Serial.println(
    "LoRa frequency: 433 MHz");

  Serial.println();
}


// =====================================================
// LOOP
// =====================================================

void loop() {

  // ===================================================
  // LVGL TICK
  // ===================================================

  static uint32_t lastTick =
    millis();


  uint32_t now =
    millis();


  lv_tick_inc(
    now - lastTick);


  lastTick =
    now;


  // ===================================================
  // LVGL HANDLER
  // ===================================================

  lv_timer_handler();


  // ===================================================
  // EEZ STUDIO TICK
  // ===================================================

  ui_tick();


  // ===================================================
  // SENSOR CYCLE
  // EVERY 5 SECONDS
  // ===================================================

  if (
    millis() - lastSensorCycle >= SENSOR_INTERVAL) {

    lastSensorCycle =
      millis();


    Serial.println();

    Serial.println(
      "================================");

    Serial.println(
      "NEW 5-SECOND SENSOR CYCLE");

    Serial.println(
      "================================");


    // -------------------------------------------------
    // READ SCD41
    // -------------------------------------------------

    bool scdRead =
      readSCD41();


    // -------------------------------------------------
    // READ INMP441
    // -------------------------------------------------

    bool noiseRead =
      readNoiseSensor();


    // -------------------------------------------------
    // PRINT CURRENT VALUES
    // -------------------------------------------------

    printSensorData();


    // -------------------------------------------------
    // STORE ONLY VALID CURRENT READINGS
    // -------------------------------------------------

    if (
      scdRead && noiseRead) {

      storeSensorData();

    } else {

      Serial.println(
        "Current reading incomplete.");

      Serial.println(
        "Reading NOT stored.");
    }


    // -------------------------------------------------
    // CHECK FOR 12 READINGS
    // -------------------------------------------------

    if (
      storedReadingCount >= READINGS_PER_MINUTE) {

      calculateAndSendAverage();
    }
  }


  // ===================================================
  // SMALL DELAY
  // ===================================================

  delay(1);
}