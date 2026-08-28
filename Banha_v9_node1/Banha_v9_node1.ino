/*
  =====================================================
  BANHA_v8.ino
  NODE 1 - SENSOR + DISPLAY + RECORDING CONTROL
  =====================================================

  LoRa packet types:

  START:
  NODE:1,TYPE:START

  DATA:
  NODE:1,TYPE:DATA,PACKET:1,AVG_CO2:842.5,AVG_TEMP:29.8,AVG_NOISE:51.8

  STOP:
  NODE:1,TYPE:STOP
*/

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
#include "screens.h"


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
// RECORDING STATUS
// =====================================================

bool isRecording = false;


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

const float QUIET_RMS = 118.6f;

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

#define READINGS_PER_MINUTE 12

float co2Readings[READINGS_PER_MINUTE];

float tempReadings[READINGS_PER_MINUTE];

float noiseReadings[READINGS_PER_MINUTE];

int storedReadingCount = 0;


// =====================================================
// FUNCTION DECLARATIONS
// =====================================================

void clearStoredData();

void startRecording();

void stopRecording();

void startRecordingEvent(lv_event_t *e);

void stopRecordingEvent(lv_event_t *e);

void initLoRa();

bool sendLoRaMessage(String message);

void sendStartCommand();

void sendStopCommand();

void calculateAndSendAverage();


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

    loraAvailable = false;

    return;
  }


  Serial.println(
    "LoRa Ready!");

  loraAvailable = true;
}


// =====================================================
// GENERIC LORA SEND
// =====================================================

bool sendLoRaMessage(
  String message) {

  if (!loraAvailable) {

    Serial.println(
      "LoRa unavailable. Message not sent.");

    return false;
  }


  Serial.println();

  Serial.println(
    "LORA TRANSMITTING:");

  Serial.println(
    message);


  LoRa.beginPacket();

  LoRa.print(
    message);


  int result =
    LoRa.endPacket();


  if (result == 1) {

    Serial.println(
      "LORA SENT SUCCESSFULLY!");

    return true;

  } else {

    Serial.println(
      "LORA SEND FAILED!");

    return false;
  }
}


// =====================================================
// SEND START COMMAND
// =====================================================

void sendStartCommand() {

  String message =
    "NODE:1,TYPE:START";


  Serial.println(
    "Sending START command to Node 2...");


  sendLoRaMessage(
    message);
}


// =====================================================
// SEND STOP COMMAND
// =====================================================

void sendStopCommand() {

  String message =
    "NODE:1,TYPE:STOP";


  Serial.println(
    "Sending STOP command to Node 2...");


  sendLoRaMessage(
    message);
}


// =====================================================
// START RECORDING
// =====================================================

void startRecording() {

  if (isRecording) {

    Serial.println(
      "Recording is already active.");

    return;
  }


  Serial.println();

  Serial.println(
    "================================");

  Serial.println(
    "START RECORDING BUTTON PRESSED");

  Serial.println(
    "Sending START command to Node 2");

  Serial.println(
    "================================");


  // -----------------------------------------------
  // RESET SESSION PACKET NUMBER
  // Every new recording starts from PACKET:1
  // -----------------------------------------------

  loraPacketNumber = 0;


  Serial.println(
    "Packet number reset to 0."
  );


  // -----------------------------------------------
  // CLEAR OLD SENSOR DATA
  // -----------------------------------------------

  clearStoredData();


  // -----------------------------------------------
  // SEND START TO NODE 2
  // -----------------------------------------------

  sendStartCommand();


  // -----------------------------------------------
  // ENABLE LOCAL RECORDING
  // -----------------------------------------------

  isRecording = true;


  // -----------------------------------------------
  // RESET SENSOR TIMER
  // -----------------------------------------------

  lastSensorCycle =
    millis();


  Serial.println(
    "RECORDING MODE ENABLED"
  );


  Serial.println(
    "First completed 1-minute average will use PACKET:1"
  );
}


// =====================================================
// STOP RECORDING
// =====================================================

void stopRecording() {

  if (!isRecording) {

    Serial.println(
      "System already in IDLE mode.");

    return;
  }


  Serial.println();

  Serial.println(
    "================================");

  Serial.println(
    "STOP RECORDING BUTTON PRESSED");

  Serial.println(
    "Sending STOP command to Node 2");

  Serial.println(
    "================================");


  // -----------------------------------------------
  // STOP LOCAL RECORDING FIRST
  // -----------------------------------------------

  isRecording = false;


  // -----------------------------------------------
  // CLEAR INCOMPLETE DATA
  // -----------------------------------------------

  clearStoredData();


  // -----------------------------------------------
  // SEND STOP TO NODE 2
  // -----------------------------------------------

  sendStopCommand();


  // -----------------------------------------------
  // RESET TIMER
  // -----------------------------------------------

  lastSensorCycle =
    millis();


  Serial.println(
    "RECORDING MODE DISABLED");

  Serial.println(
    "RETURNING TO IDLE MODE");
}


// =====================================================
// START BUTTON EVENT
// =====================================================

void startRecordingEvent(
  lv_event_t *e) {

  if (
    lv_event_get_code(e) != LV_EVENT_CLICKED) {

    return;
  }


  startRecording();
}


// =====================================================
// STOP BUTTON EVENT
// =====================================================

void stopRecordingEvent(
  lv_event_t *e) {

  if (
    lv_event_get_code(e) != LV_EVENT_CLICKED) {

    return;
  }


  stopRecording();
}


// =====================================================
// CLEAR TEMPORARY DATA
// =====================================================

void clearStoredData() {

  for (
    int i = 0;
    i < READINGS_PER_MINUTE;
    i++) {

    co2Readings[i] = 0.0f;

    tempReadings[i] = 0.0f;

    noiseReadings[i] = 0.0f;
  }


  storedReadingCount = 0;
}


// =====================================================
// STORE SENSOR DATA
// =====================================================

void storeSensorData() {

  if (
    storedReadingCount >= READINGS_PER_MINUTE) {

    return;
  }


  if (!scd41HasData) {

    Serial.println(
      "SCD41 data unavailable.");

    return;
  }


  if (!noiseHasData) {

    Serial.println(
      "Noise data unavailable.");

    return;
  }


  co2Readings[storedReadingCount] =
    latestCO2;

  tempReadings[storedReadingCount] =
    latestTemp;

  noiseReadings[storedReadingCount] =
    latestNoise;


  storedReadingCount++;


  Serial.print(
    "RECORDING READING: ");

  Serial.print(
    storedReadingCount);

  Serial.print(
    " / ");

  Serial.println(
    READINGS_PER_MINUTE);
}


// =====================================================
// CALCULATE AND SEND AVERAGE
// =====================================================

void calculateAndSendAverage() {

  if (
    storedReadingCount < READINGS_PER_MINUTE) {

    return;
  }


  if (!isRecording) {

    clearStoredData();

    return;
  }


  float totalCO2 = 0.0f;

  float totalTemp = 0.0f;

  float totalNoise = 0.0f;


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


  float averageCO2 =
    totalCO2 / READINGS_PER_MINUTE;

  float averageTemp =
    totalTemp / READINGS_PER_MINUTE;

  float averageNoise =
    totalNoise / READINGS_PER_MINUTE;


  loraPacketNumber++;


  String message;

  message.reserve(150);


  message +=
    "NODE:1";

  message +=
    ",TYPE:DATA";

  message +=
    ",PACKET:";

  message +=
    String(loraPacketNumber);

  message +=
    ",AVG_CO2:";

  message +=
    String(averageCO2, 1);

  message +=
    ",AVG_TEMP:";

  message +=
    String(averageTemp, 1);

  message +=
    ",AVG_NOISE:";

  message +=
    String(averageNoise, 1);


  Serial.println();

  Serial.println(
    "SENDING 1-MINUTE DATA:");

  Serial.println(
    message);


  sendLoRaMessage(
    message);


  clearStoredData();
}


// =====================================================
// INITIALIZE INMP441
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


  esp_err_t result =
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


  i2s_zero_dma_buffer(
    I2S_PORT);


  Serial.println(
    "INMP441 Ready");
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

    Serial.println(
      "I2S Read Error");

    return false;
  }


  int sampleCount =
    bytesRead / sizeof(int32_t);


  if (sampleCount <= 0) {

    return false;
  }


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


  float dB =
    35.0f + 20.0f * log10(rms / QUIET_RMS);


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


  set_var_noise_value(
    latestNoise);


  if (
    latestNoise <= 35.0f) {

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


  latestCO2 =
    co2;


  latestTemp =
    roundf(temp * 10.0f) / 10.0f;


  latestHumidity =
    humidity;


  scd41HasData =
    true;


  set_var_co2_value(
    latestCO2);

  set_var_temp_value(
    latestTemp);


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
    "CO2: ");

  Serial.println(
    latestCO2);


  Serial.print(
    "Temp: ");

  Serial.println(
    latestTemp,
    1);


  Serial.print(
    "Noise: ");

  Serial.println(
    latestNoise,
    1);


  Serial.print(
    "Mode: ");

  if (isRecording) {

    Serial.println(
      "RECORDING");

  } else {

    Serial.println(
      "IDLE");
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


  Wire.begin(
    22,
    21);


  scd4x.begin(
    Wire,
    0x62);


  uint16_t error;


  error =
    scd4x.stopPeriodicMeasurement();


  if (error) {

    Serial.print(
      "Stop measurement error: ");

    Serial.println(
      error);
  }


  delay(500);


  error =
    scd4x.startPeriodicMeasurement();


  if (error) {

    Serial.print(
      "SCD41 start error: ");

    Serial.println(
      error);

    scd41Available =
      false;

    return;
  }


  scd41Available =
    true;


  Serial.println(
    "SCD41 Ready");
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
    "BANHA NODE 1 SENSOR SYSTEM");

  Serial.println(
    "==================================");


  isRecording = false;

  clearStoredData();


  initSCD41();

  initINMP441();

  initLoRa();


  Serial.println(
    "Starting TFT...");

  tft.begin();

  tft.setRotation(
    1);

  tft.fillScreen(
    TFT_BLACK);


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
    "Starting LVGL...");


  lv_init();


  lv_disp_draw_buf_init(
    &draw_buf,
    buf1,
    NULL,
    SCREEN_W * 20);


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


  static lv_indev_drv_t indev_drv;

  lv_indev_drv_init(
    &indev_drv);


  indev_drv.type =
    LV_INDEV_TYPE_POINTER;

  indev_drv.read_cb =
    touch_read;


  lv_indev_drv_register(
    &indev_drv);


  Serial.println(
    "Starting EEZ UI...");


  ui_init();


  if (
    objects.btn_start_rec != NULL) {

    lv_obj_add_event_cb(
      objects.btn_start_rec,
      startRecordingEvent,
      LV_EVENT_CLICKED,
      NULL);

  } else {

    Serial.println(
      "ERROR: btn_start_rec not found.");
  }


  if (
    objects.btn_stop_rec != NULL) {

    lv_obj_add_event_cb(
      objects.btn_stop_rec,
      stopRecordingEvent,
      LV_EVENT_CLICKED,
      NULL);

  } else {

    Serial.println(
      "ERROR: btn_stop_rec not found.");
  }


  lastSensorCycle =
    millis();


  Serial.println();

  Serial.println(
    "BANHA SYSTEM READY");

  Serial.println(
    "CURRENT MODE: IDLE");
}


// =====================================================
// LOOP
// =====================================================

void loop() {

  static uint32_t lastTick =
    millis();


  uint32_t now =
    millis();


  lv_tick_inc(
    now - lastTick);


  lastTick =
    now;


  lv_timer_handler();

  ui_tick();


  if (
    millis() - lastSensorCycle >= SENSOR_INTERVAL) {

    lastSensorCycle =
      millis();


    bool scdRead =
      readSCD41();


    bool noiseRead =
      readNoiseSensor();


    printSensorData();


    if (isRecording) {

      if (
        scdRead && noiseRead) {

        storeSensorData();

      } else {

        Serial.println(
          "Incomplete reading. Not stored.");
      }


      if (
        storedReadingCount >= READINGS_PER_MINUTE) {

        calculateAndSendAverage();
      }
    }
  }


  delay(1);
}