/*
  =====================================================
  BANHA_v11_FIXED_HSPI.ino
  NODE 1 - SENSOR + DISPLAY + RECORDING CONTROL

  WHAT CHANGED FROM v9 (SPI BUS FIX)
  =====================================================
  In v9, LoRa was initialized on VSPI (SCK18/MISO19/MOSI23)
  - the SAME bus the TFT uses - and, unlike v8, LoRa is now
  polled continuously every loop() via pollIncomingLoRa()
  (LoRa.receive() + LoRa.parsePacket()) so it can catch ACKs
  at any time. That means LoRa and the TFT/LVGL flush were
  both actively fighting over VSPI on every single loop
  iteration, not just once at startup. This is a WORSE
  version of the "LoRa initialization FAILED" / bus
  contention problem than v8 had.

  FIX: LoRa now shares the HSPI bus with the touchscreen
  (touchSPI) instead of VSPI. Touch is already polled every
  loop() pass too (via LVGL's indev read callback) and
  coexists fine with LoRa because both libraries use proper
  SPI.beginTransaction()/endTransaction() calls with their
  own dedicated CS pins. TFT keeps VSPI entirely to itself.

  NEW LoRa WIRING:
    SCK   -> GPIO14   (shared HSPI, same wire as Touch T_CLK)
    MISO  -> GPIO25   (shared HSPI, same wire as Touch T_DO)
    MOSI  -> GPIO26   (shared HSPI, same wire as Touch T_DIN)
    NSS   -> GPIO13   (unchanged, dedicated to LoRa)
    RESET -> GPIO32   (unchanged, dedicated to LoRa)
    DIO0  -> GPIO34   (unchanged, dedicated to LoRa)

  INIT ORDER CHANGE:
  initLoRa() now runs AFTER touchSPI.begin() (the Touch step),
  since it reuses that already-configured bus. TFT is
  initialized first as before; LoRa is initialized LAST,
  same overall philosophy as v8, just with a different
  (safe) bus to land on.

  Everything else - the v9 reliable TX/ACK/retry queue,
  continuous 5-second noise averaging, noise calibration
  constants, LVGL UI, SCD41 handling - is UNCHANGED from v9.
  =====================================================

  LoRa packet types (Node 1 -> Node 2):

  START:
  NODE:1,TYPE:START,SEQ:<n>

  DATA:
  NODE:1,TYPE:DATA,PACKET:<n>,SEQ:<n>,AVG_CO2:842.5,AVG_TEMP:29.8,AVG_NOISE:51.8

  STOP:
  NODE:1,TYPE:STOP,SEQ:<n>

  ACK (Node 2 -> Node 1):

  NODE:2,TYPE:ACK,SEQ:<n>,ACKTYPE:<START|STOP|DATA>
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
#include <stdlib.h>

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
//
// LoRa now shares the HSPI bus (touchSPI) instead of
// VSPI. It only needs its own dedicated control pins;
// SCK/MISO/MOSI come from touchSPI, already initialized
// during the Touch setup step (see setup() below).
// =====================================================

#define LORA_SS 13
#define LORA_RST 32
#define LORA_DIO0 34

#define LORA_BAND 433E6


// =====================================================
// LORA STATUS
// =====================================================

bool loraAvailable = false;

// Session-scoped DATA packet counter (resets to 0 every
// new recording). This is the PACKET field Node 2 stores
// in Supabase - unrelated to the transport-level SEQ.
unsigned long loraPacketNumber = 0;

// Ever-increasing transport sequence number used for
// ACK / duplicate detection. Never resets.
uint32_t txSeqCounter = 0;


// =====================================================
// RELIABLE TX QUEUE / STATE MACHINE
// =====================================================

struct OutMessage {
  String payload;  // message WITHOUT the trailing ",SEQ:n"
  uint32_t seq;
  String ackType;  // "START", "STOP", or "DATA" (for logging/matching)
};

// Commands (START/STOP) get their own small queue so a
// button press is never stuck behind a DATA retry.
#define CMD_QUEUE_SIZE 3
OutMessage cmdQueue[CMD_QUEUE_SIZE];
int cmdQueueHead = 0, cmdQueueTail = 0, cmdQueueCount = 0;

// DATA packets get a separate queue.
#define DATA_QUEUE_SIZE 3
OutMessage dataQueue[DATA_QUEUE_SIZE];
int dataQueueHead = 0, dataQueueTail = 0, dataQueueCount = 0;

enum TxState { TX_IDLE,
               TX_WAITING_ACK };

TxState txState = TX_IDLE;

OutMessage currentTx;
int txAttempt = 0;

unsigned long txLastSendMillis = 0;

const int TX_MAX_ATTEMPTS = 5;
const unsigned long TX_ACK_TIMEOUT_MS = 350;  // per-attempt wait for ACK


// =====================================================
// RECORDING STATUS
// =====================================================

bool isRecording = false;


// =====================================================
// TOUCH
//
// touchSPI is HSPI. LoRa (see above) now reuses this
// same SPIClass instance rather than starting its own
// bus, so it no longer contends with the TFT's VSPI bus.
// =====================================================

#define TOUCH_SCK 14
#define TOUCH_MISO 25
#define TOUCH_MOSI 26
#define TOUCH_CS 27
#define TOUCH_IRQ 33

SPIClass touchSPI(HSPI);

XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);


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

// Small chunk read every loop() pass - keeps each I2S
// read call fast so it never blocks LVGL/touch/LoRa.
#define NOISE_CHUNK_SAMPLES 256


// =====================================================
// NOISE CALIBRATION
//
// The INMP441 is a MEMS digital microphone; it does NOT
// output a calibrated dBA value on its own. The formula
// below produces an ESTIMATED sound level using the
// standard SPL relationship (20*log10 of a pressure-like
// ratio), anchored to two constants you should tune once
// for your actual hardware/room:
//
//   NOISE_REF_RMS  - the raw RMS value this code measures
//                     while the room is verified quiet
//                     (e.g. empty room, ~30-35 dB on a
//                     phone SPL meter / commercial meter).
//   NOISE_REF_DB   - the real-world dB level that quiet
//                     baseline corresponds to.
//
// TO CALIBRATE:
//   1. Sit the device in a genuinely quiet, empty
//      classroom.
//   2. Watch the Serial Monitor "Noise RMS (raw)" value
//      printed each cycle and let it settle.
//   3. Set NOISE_REF_RMS to that settled value, and
//      NOISE_REF_DB to the real quiet-room dB level you
//      are treating as the baseline (30 is a reasonable
//      "very quiet classroom" default).
//   4. Optionally check the reading against a phone SPL
//      app while someone talks/discusses in the room and
//      nudge NOISE_REF_RMS slightly until the estimated
//      values track reasonably.
//
// Until you do that, treat AVG_NOISE as an ESTIMATED,
// relative sound level - useful for spotting
// quiet/normal/loud trends, not a certified dBA reading.
// =====================================================

const float NOISE_REF_RMS = 140.0f;
const float NOISE_REF_DB = 30.0f;

// Classification thresholds tuned for a classroom, using
// the calibration above:
//   <= 45 dB  -> NORMAL   (very quiet to quiet discussion)
//   <= 65 dB  -> MODERATE (normal classroom discussion)
//   >  65 dB  -> POOR     (loud classroom)
const float NOISE_THRESHOLD_NORMAL = 45.0f;
const float NOISE_THRESHOLD_MODERATE = 65.0f;

// Smoothing factor for the final per-5-second dB value
// (higher = more responsive, lower = more stable).
const float NOISE_SMOOTHING_ALPHA = 0.35f;

float latestNoise = 0.0f;
float latestRMS = 0.0f;
bool noiseHasData = false;

bool noiseSmoothInitialized = false;
float noiseSmoothedValue = 0.0f;

// Running accumulator for the CURRENT 5-second window.
double noiseAccumSumSquares = 0.0;
unsigned long noiseAccumCount = 0;


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
bool sendLoRaRaw(String message);
String getPacketValue(String message, String key);

bool enqueueCommand(String basePayload, String ackType);
bool enqueueData(String basePayload);
void manageTxQueue();
void pollIncomingLoRa();
void sendCurrentTx(bool isRetry);
bool isAckMatch(String message, uint32_t seq);

void calculateAndSendAverage();

void sampleNoiseContinuous();
bool finalizeNoiseReading();


// =====================================================
// DISPLAY FLUSH
// =====================================================

static void disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}


// =====================================================
// TOUCH READ
// =====================================================

static void touch_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data) {
  if (!ts.touched()) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }

  TS_Point p = ts.getPoint();

  int x = map(p.y, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX, 0, SCREEN_W - 1);
  int y = map(p.x, TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX, 0, SCREEN_H - 1);

  x = constrain(x, 0, SCREEN_W - 1);
  y = constrain(y, 0, SCREEN_H - 1);

  data->state = LV_INDEV_STATE_PR;
  data->point.x = x;
  data->point.y = y;
}


// =====================================================
// INITIALIZE LORA
// =====================================================

void initLoRa() {
  Serial.println("Starting LoRa...");

  /*
    UPDATED:

    LoRa no longer starts its own VSPI bus. It reuses the
    HSPI bus (touchSPI) that was already started during
    the Touch init step, via LoRa.setSPI(touchSPI). This
    call MUST happen after touchSPI.begin() has already
    run (see setup() below) so the bus is configured and
    ready before LoRa attaches to it.

    LoRa keeps its own dedicated NSS/RESET/DIO0 pins, so
    it can safely coexist with the touch controller on
    the same physical bus - each device only asserts its
    own CS pin during its own transaction. This matters
    even more in v9 than v8, since LoRa now stays in
    continuous receive mode and is polled every loop()
    pass to catch ACKs, same as Touch is polled every
    loop() pass for LVGL input.
  */

  LoRa.setSPI(touchSPI);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_BAND)) {
    Serial.println("LoRa initialization FAILED!");
    loraAvailable = false;
    return;
  }

  Serial.println("LoRa Ready!");
  Serial.println("Bus: shared HSPI (SCK14 / MISO25 / MOSI26)");
  loraAvailable = true;

  // Default to listening so we can catch ACKs from Node 2
  // at any time between our own transmissions.
  LoRa.receive();
}


// =====================================================
// GENERIC LORA SEND (LOW LEVEL, NOT RELIABLE BY ITSELF)
// =====================================================

bool sendLoRaRaw(String message) {
  if (!loraAvailable) {
    Serial.println("[TX] LoRa unavailable. Message not sent.");
    return false;
  }

  Serial.println();
  Serial.print("[TX] Sending: ");
  Serial.println(message);

  LoRa.beginPacket();
  LoRa.print(message);
  int result = LoRa.endPacket();

  // Go back to listening for the ACK right away.
  LoRa.receive();

  if (result == 1) {
    Serial.println("[TX] Radio reported SEND OK.");
    return true;
  } else {
    Serial.println("[TX] Radio reported SEND FAILED.");
    return false;
  }
}


// =====================================================
// GET VALUE FROM LORA PACKET
// =====================================================

String getPacketValue(String message, String key) {
  int startIndex = message.indexOf(key);
  if (startIndex == -1) return "";

  startIndex += key.length();

  int endIndex = message.indexOf(",", startIndex);
  if (endIndex == -1) endIndex = message.length();

  return message.substring(startIndex, endIndex);
}


// =====================================================
// ENQUEUE HELPERS
// =====================================================

bool enqueueCommand(String basePayload, String ackType) {
  if (cmdQueueCount >= CMD_QUEUE_SIZE) {
    Serial.println("[QUEUE] Command queue full! Dropping oldest is not done; message rejected.");
    return false;
  }

  txSeqCounter++;

  OutMessage m;
  m.payload = basePayload;
  m.seq = txSeqCounter;
  m.ackType = ackType;

  cmdQueue[cmdQueueTail] = m;
  cmdQueueTail = (cmdQueueTail + 1) % CMD_QUEUE_SIZE;
  cmdQueueCount++;

  Serial.print("[QUEUE] Queued ");
  Serial.print(ackType);
  Serial.print(" command, SEQ:");
  Serial.println(m.seq);

  return true;
}

bool enqueueData(String basePayload) {
  if (dataQueueCount >= DATA_QUEUE_SIZE) {
    Serial.println("[QUEUE] DATA queue full! Dropping oldest queued DATA packet to make room.");
    // Drop the oldest queued (not yet sent) data packet.
    dataQueueHead = (dataQueueHead + 1) % DATA_QUEUE_SIZE;
    dataQueueCount--;
  }

  txSeqCounter++;

  OutMessage m;
  m.payload = basePayload;
  m.seq = txSeqCounter;
  m.ackType = "DATA";

  dataQueue[dataQueueTail] = m;
  dataQueueTail = (dataQueueTail + 1) % DATA_QUEUE_SIZE;
  dataQueueCount++;

  Serial.print("[QUEUE] Queued DATA packet, SEQ:");
  Serial.println(m.seq);

  return true;
}


// =====================================================
// SEND START / STOP COMMANDS (QUEUED + RELIABLE)
// =====================================================

void sendStartCommand() {
  Serial.println("Queuing START command for Node 2...");
  enqueueCommand("NODE:1,TYPE:START", "START");
}

void sendStopCommand() {
  Serial.println("Queuing STOP command for Node 2...");
  enqueueCommand("NODE:1,TYPE:STOP", "STOP");
}


// =====================================================
// RELIABLE TX STATE MACHINE
// =====================================================

void sendCurrentTx(bool isRetry) {
  String message = currentTx.payload + ",SEQ:" + String(currentTx.seq);

  if (isRetry) {
    Serial.print("[RETRY] ");
    Serial.print(currentTx.ackType);
    Serial.print(" SEQ:");
    Serial.print(currentTx.seq);
    Serial.print(" attempt ");
    Serial.print(txAttempt + 1);
    Serial.print("/");
    Serial.println(TX_MAX_ATTEMPTS);
  }

  sendLoRaRaw(message);

  txState = TX_WAITING_ACK;
  txLastSendMillis = millis();
}

bool isAckMatch(String message, uint32_t seq) {
  if (!message.startsWith("NODE:2,TYPE:ACK")) return false;

  String seqStr = getPacketValue(message, "SEQ:");
  if (seqStr.length() == 0) return false;

  uint32_t ackSeq = (uint32_t)strtoul(seqStr.c_str(), NULL, 10);
  return ackSeq == seq;
}

// Called every loop() iteration. Drains any waiting LoRa
// packet and, if we are waiting on an ACK, checks whether
// it matches the message currently in flight.
void pollIncomingLoRa() {
  int packetSize = LoRa.parsePacket();
  if (packetSize <= 0) return;

  String message = "";
  while (LoRa.available()) {
    message += (char)LoRa.read();
  }

  Serial.print("[RX] ");
  Serial.println(message);

  if (txState == TX_WAITING_ACK && isAckMatch(message, currentTx.seq)) {
    Serial.print("[ACK] Confirmed ");
    Serial.print(currentTx.ackType);
    Serial.print(" SEQ:");
    Serial.println(currentTx.seq);

    txState = TX_IDLE;
  } else {
    Serial.println("[RX] Ignoring unrelated/stale packet.");
  }
}

// Called every loop() iteration. Handles dequeuing the
// next message when idle, and retry/timeout logic when
// waiting for an ACK. Commands are prioritized over DATA.
void manageTxQueue() {
  if (txState == TX_IDLE) {
    if (cmdQueueCount > 0) {
      currentTx = cmdQueue[cmdQueueHead];
      cmdQueueHead = (cmdQueueHead + 1) % CMD_QUEUE_SIZE;
      cmdQueueCount--;

      txAttempt = 0;
      sendCurrentTx(false);
      return;
    }

    if (dataQueueCount > 0) {
      currentTx = dataQueue[dataQueueHead];
      dataQueueHead = (dataQueueHead + 1) % DATA_QUEUE_SIZE;
      dataQueueCount--;

      txAttempt = 0;
      sendCurrentTx(false);
      return;
    }

    return;
  }

  // TX_WAITING_ACK
  if (millis() - txLastSendMillis >= TX_ACK_TIMEOUT_MS) {
    txAttempt++;

    if (txAttempt >= TX_MAX_ATTEMPTS) {
      Serial.print("[GIVE UP] No ACK for ");
      Serial.print(currentTx.ackType);
      Serial.print(" SEQ:");
      Serial.print(currentTx.seq);
      Serial.println(" after max attempts. Moving on.");

      txState = TX_IDLE;
      return;
    }

    sendCurrentTx(true);
  }
}


// =====================================================
// START RECORDING
// =====================================================

void startRecording() {
  if (isRecording) {
    Serial.println("Recording is already active.");
    return;
  }

  Serial.println();
  Serial.println("================================");
  Serial.println("START RECORDING BUTTON PRESSED");
  Serial.println("Queuing START command to Node 2");
  Serial.println("================================");

  // Reset session packet number - every new recording
  // starts from PACKET:1
  loraPacketNumber = 0;
  Serial.println("Packet number reset to 0.");

  clearStoredData();

  sendStartCommand();

  isRecording = true;

  lastSensorCycle = millis();

  Serial.println("RECORDING MODE ENABLED");
  Serial.println("First completed 1-minute average will use PACKET:1");
}


// =====================================================
// STOP RECORDING
// =====================================================

void stopRecording() {
  if (!isRecording) {
    Serial.println("System already in IDLE mode.");
    return;
  }

  Serial.println();
  Serial.println("================================");
  Serial.println("STOP RECORDING BUTTON PRESSED");
  Serial.println("Queuing STOP command to Node 2");
  Serial.println("================================");

  isRecording = false;

  clearStoredData();

  sendStopCommand();

  lastSensorCycle = millis();

  Serial.println("RECORDING MODE DISABLED");
  Serial.println("RETURNING TO IDLE MODE");
}


// =====================================================
// START / STOP BUTTON EVENTS
// =====================================================

void startRecordingEvent(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  startRecording();
}

void stopRecordingEvent(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  stopRecording();
}


// =====================================================
// CLEAR TEMPORARY DATA
// =====================================================

void clearStoredData() {
  for (int i = 0; i < READINGS_PER_MINUTE; i++) {
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
  if (storedReadingCount >= READINGS_PER_MINUTE) return;

  if (!scd41HasData) {
    Serial.println("SCD41 data unavailable.");
    return;
  }

  if (!noiseHasData) {
    Serial.println("Noise data unavailable.");
    return;
  }

  co2Readings[storedReadingCount] = latestCO2;
  tempReadings[storedReadingCount] = latestTemp;
  noiseReadings[storedReadingCount] = latestNoise;

  storedReadingCount++;

  Serial.print("RECORDING READING: ");
  Serial.print(storedReadingCount);
  Serial.print(" / ");
  Serial.println(READINGS_PER_MINUTE);
}


// =====================================================
// CALCULATE AND SEND AVERAGE (QUEUED + RELIABLE)
// =====================================================

void calculateAndSendAverage() {
  if (storedReadingCount < READINGS_PER_MINUTE) return;

  if (!isRecording) {
    clearStoredData();
    return;
  }

  float totalCO2 = 0.0f;
  float totalTemp = 0.0f;
  float totalNoise = 0.0f;

  for (int i = 0; i < READINGS_PER_MINUTE; i++) {
    totalCO2 += co2Readings[i];
    totalTemp += tempReadings[i];
    totalNoise += noiseReadings[i];
  }

  float averageCO2 = totalCO2 / READINGS_PER_MINUTE;
  float averageTemp = totalTemp / READINGS_PER_MINUTE;
  float averageNoise = totalNoise / READINGS_PER_MINUTE;

  loraPacketNumber++;

  String message;
  message.reserve(150);

  message += "NODE:1";
  message += ",TYPE:DATA";
  message += ",PACKET:";
  message += String(loraPacketNumber);
  message += ",AVG_CO2:";
  message += String(averageCO2, 1);
  message += ",AVG_TEMP:";
  message += String(averageTemp, 1);
  message += ",AVG_NOISE:";
  message += String(averageNoise, 1);

  Serial.println();
  Serial.println("QUEUING 1-MINUTE DATA PACKET:");
  Serial.println(message);

  enqueueData(message);

  clearStoredData();
}


// =====================================================
// INITIALIZE INMP441
// =====================================================

void initINMP441() {
  Serial.println("Initializing INMP441...");

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  esp_err_t result = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (result != ESP_OK) {
    Serial.print("I2S driver error: ");
    Serial.println(result);
    return;
  }

  result = i2s_set_pin(I2S_PORT, &pin_config);
  if (result != ESP_OK) {
    Serial.print("I2S pin error: ");
    Serial.println(result);
    return;
  }

  i2s_zero_dma_buffer(I2S_PORT);

  Serial.println("INMP441 Ready");
}


// =====================================================
// CONTINUOUS NOISE SAMPLING
//
// Call this every pass through loop(). It performs a
// SHORT, NON-BLOCKING I2S read (timeout of 0 ticks) and
// folds whatever samples are available into the running
// sum-of-squares for the current 5-second window. Because
// the timeout is 0, this call returns almost immediately
// even if no new audio is ready yet, so it never stalls
// LVGL, touch, LoRa, or SCD41 handling.
// =====================================================

void sampleNoiseContinuous() {
  static int32_t chunk[NOISE_CHUNK_SAMPLES];
  size_t bytesRead = 0;

  esp_err_t result = i2s_read(I2S_PORT, chunk, sizeof(chunk), &bytesRead, 0);

  if (result != ESP_OK || bytesRead == 0) {
    return;
  }

  int sampleCount = bytesRead / sizeof(int32_t);

  for (int i = 0; i < sampleCount; i++) {
    float sample = chunk[i] >> 14;
    noiseAccumSumSquares += (double)sample * (double)sample;
  }

  noiseAccumCount += sampleCount;
}


// =====================================================
// FINALIZE NOISE READING FOR THE JUST-COMPLETED
// 5-SECOND WINDOW
// =====================================================

bool finalizeNoiseReading() {
  if (noiseAccumCount == 0) {
    Serial.println("Noise: no samples collected this window.");
    noiseHasData = false;

    // Reset window regardless so the next one starts clean.
    noiseAccumSumSquares = 0.0;
    noiseAccumCount = 0;
    return false;
  }

  float rms = sqrt(noiseAccumSumSquares / (double)noiseAccumCount);
  if (rms < 1.0f) rms = 1.0f;

  float dB = NOISE_REF_DB + 20.0f * log10(rms / NOISE_REF_RMS);
  if (dB < 0.0f) dB = 0.0f;
  if (dB > 120.0f) dB = 120.0f;

  if (!noiseSmoothInitialized) {
    noiseSmoothedValue = dB;
    noiseSmoothInitialized = true;
  } else {
    noiseSmoothedValue = (NOISE_SMOOTHING_ALPHA * dB) + ((1.0f - NOISE_SMOOTHING_ALPHA) * noiseSmoothedValue);
  }

  latestRMS = rms;
  latestNoise = noiseSmoothedValue;
  noiseHasData = true;

  set_var_noise_value(latestNoise);

  if (latestNoise <= NOISE_THRESHOLD_NORMAL) {
    set_var_noise_status("NORMAL");
  } else if (latestNoise <= NOISE_THRESHOLD_MODERATE) {
    set_var_noise_status("MODERATE");
  } else {
    set_var_noise_status("POOR");
  }

  Serial.print("Noise RMS (raw): ");
  Serial.print(rms, 1);
  Serial.print("  |  Estimated dB (smoothed): ");
  Serial.print(latestNoise, 1);
  Serial.print("  |  Samples this window: ");
  Serial.println(noiseAccumCount);

  // Reset for the next 5-second window.
  noiseAccumSumSquares = 0.0;
  noiseAccumCount = 0;

  return true;
}


// =====================================================
// READ SCD41
// =====================================================

bool readSCD41() {
  if (!scd41Available) return false;

  bool dataReady = false;
  uint16_t error = scd4x.getDataReadyStatus(dataReady);

  if (error) {
    Serial.print("SCD41 Data Ready Error: ");
    Serial.println(error);
    return false;
  }

  if (!dataReady) return false;

  uint16_t co2;
  float temp;
  float humidity;

  error = scd4x.readMeasurement(co2, temp, humidity);

  if (error) {
    Serial.print("SCD41 Read Error: ");
    Serial.println(error);
    return false;
  }

  latestCO2 = co2;
  latestTemp = roundf(temp * 10.0f) / 10.0f;
  latestHumidity = humidity;
  scd41HasData = true;

  set_var_co2_value(latestCO2);
  set_var_temp_value(latestTemp);

  if (latestTemp < 22.0f) {
    set_var_temp_status("POOR");
  } else if (latestTemp < 23.0f) {
    set_var_temp_status("MODERATE");
  } else if (latestTemp <= 26.0f) {
    set_var_temp_status("NORMAL");
  } else if (latestTemp <= 27.1f) {
    set_var_temp_status("MODERATE");
  } else {
    set_var_temp_status("POOR");
  }

  if (latestCO2 <= 1000) {
    set_var_co2_status("NORMAL");
  } else if (latestCO2 <= 1500) {
    set_var_co2_status("MODERATE");
  } else {
    set_var_co2_status("POOR");
  }

  return true;
}


// =====================================================
// PRINT SENSOR DATA
// =====================================================

void printSensorData() {
  Serial.println("----------------------");
  Serial.print("CO2: ");
  Serial.println(latestCO2);

  Serial.print("Temp: ");
  Serial.println(latestTemp, 1);

  Serial.print("Noise: ");
  Serial.println(latestNoise, 1);

  Serial.print("Mode: ");
  Serial.println(isRecording ? "RECORDING" : "IDLE");
  Serial.println("----------------------");
}


// =====================================================
// INITIALIZE SCD41
// =====================================================

void initSCD41() {
  Serial.println("Starting SCD41...");

  Wire.begin(22, 21);
  scd4x.begin(Wire, 0x62);

  uint16_t error;

  error = scd4x.stopPeriodicMeasurement();
  if (error) {
    Serial.print("Stop measurement error: ");
    Serial.println(error);
  }

  delay(500);

  error = scd4x.startPeriodicMeasurement();
  if (error) {
    Serial.print("SCD41 start error: ");
    Serial.println(error);
    scd41Available = false;
    return;
  }

  scd41Available = true;
  Serial.println("SCD41 Ready");
}


// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("==================================");
  Serial.println("BANHA NODE 1 SENSOR SYSTEM");
  Serial.println("==================================");

  isRecording = false;
  clearStoredData();

  // ===================================================
  // 1. SCD41
  // ===================================================
  initSCD41();

  // ===================================================
  // 2. INMP441
  // ===================================================
  initINMP441();

  // ===================================================
  // 3. TFT
  // ===================================================
  Serial.println("Starting TFT...");
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  // ===================================================
  // 4. TOUCH
  //
  // touchSPI.begin() starts the HSPI bus here. LoRa
  // (step 8, below) will reuse this exact bus instance,
  // so this step must run before initLoRa().
  // ===================================================
  Serial.println("Starting Touch...");
  touchSPI.begin(TOUCH_SCK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(0);

  // ===================================================
  // 5. LVGL
  // ===================================================
  Serial.println("Starting LVGL...");
  lv_init();

  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SCREEN_W * 20);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_W;
  disp_drv.ver_res = SCREEN_H;
  disp_drv.flush_cb = disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  // ===================================================
  // 6. TOUCH INPUT FOR LVGL
  // ===================================================
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read;
  lv_indev_drv_register(&indev_drv);

  // ===================================================
  // 7. EEZ UI
  // ===================================================
  Serial.println("Starting EEZ UI...");
  ui_init();

  if (objects.btn_start_rec != NULL) {
    lv_obj_add_event_cb(objects.btn_start_rec, startRecordingEvent, LV_EVENT_CLICKED, NULL);
  } else {
    Serial.println("ERROR: btn_start_rec not found.");
  }

  if (objects.btn_stop_rec != NULL) {
    lv_obj_add_event_cb(objects.btn_stop_rec, stopRecordingEvent, LV_EVENT_CLICKED, NULL);
  } else {
    Serial.println("ERROR: btn_stop_rec not found.");
  }

  // ===================================================
  // 8. LORA LAST
  //
  // UPDATED: LoRa is initialized after TFT/Touch/LVGL/
  // EEZ UI, and attaches to the already-running HSPI bus
  // (touchSPI) inside initLoRa(), which is fully
  // independent of the TFT's VSPI bus.
  // ===================================================
  initLoRa();

  lastSensorCycle = millis();

  Serial.println();
  Serial.println("BANHA SYSTEM READY");
  Serial.println("CURRENT MODE: IDLE");
}


// =====================================================
// LOOP
// =====================================================

void loop() {
  static uint32_t lastTick = millis();
  uint32_t now = millis();

  lv_tick_inc(now - lastTick);
  lastTick = now;

  lv_timer_handler();
  ui_tick();

  // ---------------------------------------------------
  // Continuously fold in noise samples for this
  // 5-second window (short, non-blocking read).
  // ---------------------------------------------------
  sampleNoiseContinuous();

  // ---------------------------------------------------
  // Reliable LoRa: drain any incoming packet (checks it
  // against the message we're currently waiting to have
  // ACKed), then advance the send queue / retry timers.
  // ---------------------------------------------------
  pollIncomingLoRa();
  manageTxQueue();

  // ---------------------------------------------------
  // Every 5 seconds: finalize noise + read SCD41 + store
  // ---------------------------------------------------
  if (millis() - lastSensorCycle >= SENSOR_INTERVAL) {
    lastSensorCycle = millis();

    bool scdRead = readSCD41();
    bool noiseRead = finalizeNoiseReading();

    printSensorData();

    if (isRecording) {
      if (scdRead && noiseRead) {
        storeSensorData();
      } else {
        Serial.println("Incomplete reading. Not stored.");
      }

      if (storedReadingCount >= READINGS_PER_MINUTE) {
        calculateAndSendAverage();
      }
    }
  }

  delay(1);
}
