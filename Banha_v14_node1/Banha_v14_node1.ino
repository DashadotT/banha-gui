/*
  =====================================================
  BANHA_v14.ino
  NODE 1 - LoRa moved to its own task (touch fix, take 2)

  WHY v13's APPROACH DID NOT COMPILE
  =====================================================
  v15 used LoRa.isTransmitting() to detect when an async
  send had finished, without blocking. That method exists
  in most copies of the sandeepmistry/arduino-LoRa library,
  but in the copy installed on this machine it is declared
  PRIVATE, so it can't be called from the sketch. Different
  forks/versions of this library disagree on this, so
  depending on it is fragile.

  NEW APPROACH (v14) - NO LONGER NEEDS isTransmitting() AT ALL
  =====================================================
  Instead of trying to make LoRa TX non-blocking, LoRa is
  moved into its OWN FreeRTOS task (loraTask), pinned to the
  OTHER CPU core from the Arduino loop(). All the reliable
  TX/ACK/retry queue logic, RX polling, and the heartbeat now
  run inside loraTask using plain, simple BLOCKING LoRa calls
  (exactly like the original v9/v13 code) - blocking is fine
  there, because loraTask is completely separate from the
  task that calls lv_timer_handler() / touch_read(). loop()
  itself now ONLY does LVGL/UI work and sensor sampling; it
  never touches the LoRa radio directly, so it can never be
  stalled by a LoRa transmission again, no matter how long
  that transmission or a retry storm takes.

  SPI BUS SHARING
  =====================================================
  LoRa and Touch still share the HSPI bus (touchSPI), as in
  v9 - both use proper SPI.beginTransaction()/endTransaction()
  internally, and the ESP32 SPI driver is itself safe to call
  from multiple tasks. As an extra safety net (rather than
  relying purely on that), this version adds an explicit
  FreeRTOS mutex (spiBusMutex) that both touch_read() (in the
  LVGL/main task) and the LoRa functions (in loraTask) must
  take before touching the shared bus, and release right
  after. Each side uses a short timeout and simply skips that
  one poll if the bus is momentarily busy, trying again on the
  very next cycle a few milliseconds later - so neither side
  ever waits long enough for the person to notice.

  QUEUE THREAD-SAFETY
  =====================================================
  The command/DATA queues (cmdQueue / dataQueue) are now
  written from TWO different tasks - loop() (button presses,
  and calculateAndSendAverage() from the sensor cycle) and
  loraTask (dequeuing to send, and the PING heartbeat). Their
  head/tail/count bookkeeping and txSeqCounter are now
  protected by a small critical section (queueMux), matching
  the pattern already used elsewhere in this file for the
  noise-sampling buffers.

  Everything else - the reliable TX/ACK/retry queue
  behaviour, 5-second noise averaging, noise calibration
  constants, LVGL UI, SCD41 handling - is functionally
  UNCHANGED from v14/v15.
  =====================================================

  LoRa packet types (Node 1 -> Node 2):

  START:
  NODE:1,TYPE:START,SEQ:<n>

  DATA:
  NODE:1,TYPE:DATA,PACKET:<n>,SEQ:<n>,AVG_TEMP:29.8,AVG_NOISE:51.8

  STOP:
  NODE:1,TYPE:STOP,SEQ:<n>

  ACK (Node 2 -> Node 1):

  NODE:2,TYPE:ACK,SEQ:<n>,ACKTYPE:<START|STOP|DATA|PING>

  HEARTBEAT:

  Node 1 periodically sends:
  NODE:1,TYPE:PING,SEQ:<n>

  Node 2 must reply:
  NODE:2,TYPE:ACK,SEQ:<n>,ACKTYPE:PING
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
// LoRa shares the HSPI bus (touchSPI) instead of VSPI. It
// only needs its own dedicated control pins; SCK/MISO/MOSI
// come from touchSPI, already initialized during the Touch
// setup step (see setup() below). All LoRa I/O now runs
// inside loraTask, not loop().
// =====================================================

#define LORA_SS 13
#define LORA_RST 32
#define LORA_DIO0 34

#define LORA_BAND 433E6


// =====================================================
// SHARED SPI BUS MUTEX
//
// NEW: guards every access to touchSPI, from either
// touch_read() (main/LVGL task) or the LoRa functions
// (loraTask). Both sides use a short timeout and simply
// skip that one poll if the bus is busy rather than
// blocking - the next poll a few ms later picks it back up.
// =====================================================

SemaphoreHandle_t spiBusMutex = nullptr;

const TickType_t SPI_MUTEX_TOUCH_TIMEOUT = pdMS_TO_TICKS(5);
const TickType_t SPI_MUTEX_LORA_TIMEOUT = pdMS_TO_TICKS(20);


// =====================================================
// LORA STATUS
// =====================================================

bool loraAvailable = false;

// Actual Node 1 <-> Node 2 communication status.
// This is NOT the same as loraAvailable: loraAvailable only means
// the local LoRa radio initialized successfully.
// UPDATED: written by loraTask, read by loop() for the UI label,
// so it is volatile.
volatile bool node2Connected = false;
unsigned long lastNode2AckMillis = 0;
unsigned long lastHeartbeatMillis = 0;
bool heartbeatInFlight = false;

// Heartbeat interval backs off while disconnected, instead of
// retrying every 2 seconds against an unreachable Node 2.
const unsigned long NODE2_HEARTBEAT_INTERVAL_CONNECTED_MS = 2000;
const unsigned long NODE2_HEARTBEAT_INTERVAL_DISCONNECTED_MS = 5000;
const unsigned long NODE2_CONNECTION_TIMEOUT_MS = 5000;

// Session-scoped DATA packet counter (resets to 0 every
// new recording). This is the PACKET field Node 2 stores
// in Supabase - unrelated to the transport-level SEQ.
// Owned entirely by loop() / the sensor cycle.
unsigned long loraPacketNumber = 0;

// Ever-increasing transport sequence number used for
// ACK / duplicate detection. Never resets.
// UPDATED: now touched from both loop() (START/STOP/DATA
// enqueue) and loraTask (PING enqueue), so it is only ever
// modified inside a queueMux critical section.
uint32_t txSeqCounter = 0;


// =====================================================
// RELIABLE TX QUEUE / STATE MACHINE
// =====================================================

struct OutMessage {
  String payload;  // message WITHOUT the trailing ",SEQ:n"
  uint32_t seq;
  String ackType;  // "START", "STOP", "DATA", or "PING" (for logging/matching)
};

// Commands (START/STOP) get their own small queue so a
// button press is never stuck behind a DATA retry.
#define CMD_QUEUE_SIZE 3
OutMessage cmdQueue[CMD_QUEUE_SIZE];
int cmdQueueHead = 0, cmdQueueTail = 0, cmdQueueCount = 0;

// DATA packets get a separate queue.
#define DATA_QUEUE_SIZE 60
OutMessage dataQueue[DATA_QUEUE_SIZE];
int dataQueueHead = 0, dataQueueTail = 0, dataQueueCount = 0;

// NEW: protects cmdQueue/dataQueue (head/tail/count) and
// txSeqCounter, since these are now written from both loop()
// and loraTask. Matches the noiseMux pattern already used
// elsewhere in this file.
portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;

// TxState/currentTx/txAttempt/txLastSendMillis are only ever
// touched from inside loraTask now, so they need no locking.
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

// Node/link and packet status shown on Screens 1, 2 and 3.
// Runs in loop(); reads node2Connected (volatile, written by
// loraTask) and loraPacketNumber (owned by loop()).
static void updateStatusLabels() {
  const lv_color_t nodeConnectedColor = lv_color_hex(0x00BF63);
  const lv_color_t nodeDisconnectedColor = lv_color_hex(0xFF383C);
  const lv_color_t packetIdleColor = lv_color_hex(0x858585);
  const lv_color_t packetRecordingColor = lv_color_hex(0xFFFFFF);

  bool connected = node2Connected;

  const char *nodeText = connected ? "CONNECTED" : "DISCONNECTED";
  const lv_color_t nodeColor = connected ? nodeConnectedColor : nodeDisconnectedColor;

  String packetText = isRecording ? String(loraPacketNumber) : String("NOT RECORDING");
  const lv_color_t packetColor = isRecording ? packetRecordingColor : packetIdleColor;

  if (objects.s1_nodes_status_label) {
    lv_label_set_text(objects.s1_nodes_status_label, nodeText);
    lv_obj_set_style_text_color(objects.s1_nodes_status_label, nodeColor, LV_PART_MAIN | LV_STATE_DEFAULT);
  }
  if (objects.s2_nodes_status_label) {
    lv_label_set_text(objects.s2_nodes_status_label, nodeText);
    lv_obj_set_style_text_color(objects.s2_nodes_status_label, nodeColor, LV_PART_MAIN | LV_STATE_DEFAULT);
  }
  if (objects.s3_nodes_status_label) {
    lv_label_set_text(objects.s3_nodes_status_label, nodeText);
    lv_obj_set_style_text_color(objects.s3_nodes_status_label, nodeColor, LV_PART_MAIN | LV_STATE_DEFAULT);
  }

  if (objects.s1_packet_status_label) {
    lv_label_set_text(objects.s1_packet_status_label, packetText.c_str());
    lv_obj_set_style_text_color(objects.s1_packet_status_label, packetColor, LV_PART_MAIN | LV_STATE_DEFAULT);
  }
  if (objects.s2_packet_status_label) {
    lv_label_set_text(objects.s2_packet_status_label, packetText.c_str());
    lv_obj_set_style_text_color(objects.s2_packet_status_label, packetColor, LV_PART_MAIN | LV_STATE_DEFAULT);
  }
  if (objects.s3_packet_status_label) {
    lv_label_set_text(objects.s3_packet_status_label, packetText.c_str());
    lv_obj_set_style_text_color(objects.s3_packet_status_label, packetColor, LV_PART_MAIN | LV_STATE_DEFAULT);
  }
}


// =====================================================
// TOUCH
//
// touchSPI is HSPI. LoRa (see above) reuses this same
// SPIClass instance rather than starting its own bus.
// touch_read() now runs on the main/LVGL task while LoRa
// runs on loraTask (a different core) - both take
// spiBusMutex before touching the bus.
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

// The audio sampler runs in its own FreeRTOS task so the INMP441
// I2S DMA is continuously drained instead of relying on loop() timing.
#define NOISE_CHUNK_SAMPLES 256
#define NOISE_SAMPLE_RATE 16000
#define NOISE_WINDOW_MS 5000

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

const float NOISE_REF_RMS = 500.0f;
const float NOISE_REF_DB = 30.0f;

// Classification thresholds for BANHA classroom monitoring:
//   <= 35 dB  -> NORMAL
//   >  35 to <= 55 dB -> MODERATE
//   >  55 dB  -> POOR
const float NOISE_THRESHOLD_NORMAL = 35.0f;
const float NOISE_THRESHOLD_MODERATE = 55.0f;

float latestNoise = 0.0f;
float latestRMS = 0.0f;
bool noiseHasData = false;

// Audio sampling task state.
// Two buffers let the audio task continue immediately after completing
// a 5-second measurement while the main loop consumes the previous one.
struct NoiseWindowBuffer {
  double sumSquares;
  uint32_t sampleCount;
};

portMUX_TYPE noiseMux = portMUX_INITIALIZER_UNLOCKED;
NoiseWindowBuffer noiseBuffers[2] = { { 0.0, 0 }, { 0.0, 0 } };
volatile int noiseActiveBuffer = 0;
volatile int noiseReadyBuffer = -1;
volatile bool noiseTaskRunning = true;
TaskHandle_t noiseTaskHandle = nullptr;

const uint32_t NOISE_SAMPLES_PER_WINDOW =
  NOISE_SAMPLE_RATE * (NOISE_WINDOW_MS / 1000UL);


// =====================================================
// LORA TASK HANDLE
// =====================================================

TaskHandle_t loraTaskHandle = nullptr;


// =====================================================
// SENSOR TIMING
// =====================================================

unsigned long lastSensorCycle = 0;
const unsigned long SENSOR_INTERVAL = 5000;


// =====================================================
// 1-MINUTE DATA STORAGE
// =====================================================

#define READINGS_PER_MINUTE 12

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
void manageNode2Heartbeat();
void loraTask(void *parameter);

void calculateAndSendAverage();

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
//
// UPDATED: takes spiBusMutex before touching touchSPI. If
// loraTask currently holds the bus, this waits at most
// SPI_MUTEX_TOUCH_TIMEOUT (5 ms) and then reports "not
// touched" for this one LVGL poll rather than blocking -
// the very next poll (LVGL polls this frequently) tries
// again. In practice LoRa only holds the bus for a few ms
// at a time, so this is not perceptible to the user.
// =====================================================

static void touch_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data) {
  if (spiBusMutex == nullptr || xSemaphoreTake(spiBusMutex, SPI_MUTEX_TOUCH_TIMEOUT) != pdTRUE) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }

  bool isTouched = ts.touched();

  if (!isTouched) {
    xSemaphoreGive(spiBusMutex);
    data->state = LV_INDEV_STATE_REL;
    return;
  }

  TS_Point p = ts.getPoint();

  xSemaphoreGive(spiBusMutex);

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
//
// Still called once from setup(), on the main task, before
// loraTask is created - no locking needed here since nothing
// else touches the bus yet at this point.
// =====================================================

void initLoRa() {
  Serial.println("Starting LoRa...");

  LoRa.setSPI(touchSPI);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_BAND)) {
    Serial.println("LoRa initialization FAILED!");
    loraAvailable = false;
    node2Connected = false;
    return;
  }

  Serial.println("LoRa Ready!");
  Serial.println("Bus: shared HSPI (SCK14 / MISO25 / MOSI26)");
  loraAvailable = true;
  node2Connected = false;
  heartbeatInFlight = false;
  lastNode2AckMillis = 0;
  lastHeartbeatMillis = millis() - NODE2_HEARTBEAT_INTERVAL_CONNECTED_MS;

  // Default to listening so we can catch ACKs from Node 2
  // at any time between our own transmissions.
  LoRa.receive();
}


// =====================================================
// GENERIC LORA SEND (LOW LEVEL, NOT RELIABLE BY ITSELF)
//
// UPDATED: back to a simple BLOCKING call, same as v9/v13.
// This is safe now because it only ever runs inside
// loraTask, never inside loop(), so it cannot stall
// lv_timer_handler()/touch_read(). It also takes
// spiBusMutex around the whole transaction so it never
// interleaves with a touch read mid-transmission.
// =====================================================

bool sendLoRaRaw(String message) {
  if (!loraAvailable) {
    Serial.println("[TX] LoRa unavailable. Message not sent.");
    return false;
  }

  if (spiBusMutex == nullptr || xSemaphoreTake(spiBusMutex, SPI_MUTEX_LORA_TIMEOUT) != pdTRUE) {
    Serial.println("[TX] Could not acquire SPI bus. Will retry next cycle.");
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

  xSemaphoreGive(spiBusMutex);

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
//
// UPDATED: queue mutation + txSeqCounter increment now run
// inside a queueMux critical section, since these are called
// from both loop() (buttons, sensor cycle) and loraTask
// (heartbeat PING).
// =====================================================

bool enqueueCommand(String basePayload, String ackType) {
  bool ok = false;
  uint32_t assignedSeq = 0;

  portENTER_CRITICAL(&queueMux);
  if (cmdQueueCount < CMD_QUEUE_SIZE) {
    txSeqCounter++;
    assignedSeq = txSeqCounter;

    OutMessage m;
    m.payload = basePayload;
    m.seq = assignedSeq;
    m.ackType = ackType;

    cmdQueue[cmdQueueTail] = m;
    cmdQueueTail = (cmdQueueTail + 1) % CMD_QUEUE_SIZE;
    cmdQueueCount++;
    ok = true;
  }
  portEXIT_CRITICAL(&queueMux);

  if (!ok) {
    Serial.println("[QUEUE] Command queue full! Dropping oldest is not done; message rejected.");
    return false;
  }

  Serial.print("[QUEUE] Queued ");
  Serial.print(ackType);
  Serial.print(" command, SEQ:");
  Serial.println(assignedSeq);

  return true;
}

bool enqueueData(String basePayload) {
  bool ok = false;
  uint32_t assignedSeq = 0;

  portENTER_CRITICAL(&queueMux);
  if (dataQueueCount < DATA_QUEUE_SIZE) {
    txSeqCounter++;
    assignedSeq = txSeqCounter;

    OutMessage m;
    m.payload = basePayload;
    m.seq = assignedSeq;
    m.ackType = "DATA";

    dataQueue[dataQueueTail] = m;
    dataQueueTail = (dataQueueTail + 1) % DATA_QUEUE_SIZE;
    dataQueueCount++;
    ok = true;
  }
  portEXIT_CRITICAL(&queueMux);

  if (!ok) {
    // Never discard an older DATA packet just to make room.
    // Rejecting the new packet lets the caller retain/retry it
    // instead of silently losing already queued research data.
    Serial.println("[QUEUE] DATA queue full! DATA not accepted; no queued packet was dropped.");
    return false;
  }

  Serial.print("[QUEUE] Queued DATA packet, SEQ:");
  Serial.println(assignedSeq);

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
// NODE 2 HEARTBEAT
//
// Runs inside loraTask now. Backs off to
// NODE2_HEARTBEAT_INTERVAL_DISCONNECTED_MS while the link is
// known to be down, instead of retrying every 2 seconds.
// =====================================================

void manageNode2Heartbeat() {
  if (!loraAvailable) {
    node2Connected = false;
    heartbeatInFlight = false;
    return;
  }

  // Any valid ACK from Node 2 proves the link is alive.
  if (node2Connected && millis() - lastNode2AckMillis > NODE2_CONNECTION_TIMEOUT_MS) {
    node2Connected = false;
    Serial.println("[LINK] Node 2 heartbeat timeout -> DISCONNECTED");
  }

  // Do not inject a heartbeat while another reliable packet is
  // being transmitted/retried, or while packets are waiting.
  if (txState != TX_IDLE) return;
  if (cmdQueueCount > 0 || dataQueueCount > 0) return;
  if (heartbeatInFlight) return;

  unsigned long interval = node2Connected
                             ? NODE2_HEARTBEAT_INTERVAL_CONNECTED_MS
                             : NODE2_HEARTBEAT_INTERVAL_DISCONNECTED_MS;

  if (millis() - lastHeartbeatMillis < interval) return;

  lastHeartbeatMillis = millis();

  if (enqueueCommand("NODE:1,TYPE:PING", "PING")) {
    heartbeatInFlight = true;
    Serial.println("[HEARTBEAT] PING queued for Node 2.");
  }
}


// =====================================================
// RELIABLE TX STATE MACHINE
//
// Runs inside loraTask. Back to the simple blocking-send
// model - safe here because loraTask is independent of the
// UI task.
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

// Runs inside loraTask. Drains any waiting LoRa packet and,
// if we are waiting on an ACK, checks whether it matches the
// message currently in flight. Takes spiBusMutex around the
// actual radio access.
void pollIncomingLoRa() {
  if (spiBusMutex == nullptr || xSemaphoreTake(spiBusMutex, SPI_MUTEX_LORA_TIMEOUT) != pdTRUE) {
    return;  // bus busy (touch mid-read) - try again next loraTask cycle
  }

  int packetSize = LoRa.parsePacket();

  String message = "";
  if (packetSize > 0) {
    while (LoRa.available()) {
      message += (char)LoRa.read();
    }
  }

  xSemaphoreGive(spiBusMutex);

  if (packetSize <= 0) return;

  Serial.print("[RX] ");
  Serial.println(message);

  if (txState == TX_WAITING_ACK && isAckMatch(message, currentTx.seq)) {
    Serial.print("[ACK] Confirmed ");
    Serial.print(currentTx.ackType);
    Serial.print(" SEQ:");
    Serial.println(currentTx.seq);

    txState = TX_IDLE;

    // A valid ACK from Node 2 is proof that the two nodes
    // are currently communicating. This applies to START,
    // STOP, DATA, and the PING heartbeat.
    node2Connected = true;
    lastNode2AckMillis = millis();

    if (currentTx.ackType == "PING") {
      heartbeatInFlight = false;
      Serial.println("[LINK] Node 2 responded to heartbeat -> CONNECTED");
    }
  } else {
    Serial.println("[RX] Ignoring unrelated/stale packet.");
  }
}

// Runs inside loraTask. Handles dequeuing the next message
// when idle, and retry/timeout logic when waiting for an
// ACK. Commands are prioritized over DATA.
void manageTxQueue() {
  if (txState == TX_IDLE) {
    bool haveMessage = false;

    portENTER_CRITICAL(&queueMux);
    if (cmdQueueCount > 0) {
      currentTx = cmdQueue[cmdQueueHead];
      cmdQueueHead = (cmdQueueHead + 1) % CMD_QUEUE_SIZE;
      cmdQueueCount--;
      haveMessage = true;
    } else if (dataQueueCount > 0) {
      currentTx = dataQueue[dataQueueHead];
      dataQueueHead = (dataQueueHead + 1) % DATA_QUEUE_SIZE;
      dataQueueCount--;
      haveMessage = true;
    }
    portEXIT_CRITICAL(&queueMux);

    if (haveMessage) {
      txAttempt = 0;
      sendCurrentTx(false);
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

      if (currentTx.ackType == "PING") {
        heartbeatInFlight = false;
        node2Connected = false;
        Serial.println("[LINK] No heartbeat ACK from Node 2 -> DISCONNECTED");
      }

      txState = TX_IDLE;
      return;
    }

    sendCurrentTx(true);
  }
}


// =====================================================
// LORA TASK
//
// NEW: everything LoRa-related now runs here, pinned to the
// core OPPOSITE the Arduino loop() task, so it can never
// delay lv_timer_handler()/touch_read(). A 2 ms delay each
// iteration is plenty responsive for ACK timing (350 ms
// timeout) while leaving CPU time for other tasks.
// =====================================================

void loraTask(void *parameter) {
  for (;;) {
    pollIncomingLoRa();
    manageTxQueue();
    manageNode2Heartbeat();
    vTaskDelay(pdMS_TO_TICKS(2));
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

  float totalTemp = 0.0f;
  float totalNoise = 0.0f;

  for (int i = 0; i < READINGS_PER_MINUTE; i++) {
    totalTemp += tempReadings[i];
    totalNoise += noiseReadings[i];
  }

  float averageTemp = totalTemp / READINGS_PER_MINUTE;
  float averageNoise = totalNoise / READINGS_PER_MINUTE;

  loraPacketNumber++;

  String message;
  message.reserve(120);

  message += "NODE:1";
  message += ",TYPE:DATA";
  message += ",PACKET:";
  message += String(loraPacketNumber);
  message += ",AVG_TEMP:";
  message += String(averageTemp, 1);
  message += ",AVG_NOISE:";
  message += String(averageNoise, 1);

  Serial.println();
  Serial.println("QUEUING 1-MINUTE DATA PACKET:");
  Serial.println(message);

  if (enqueueData(message)) {
    clearStoredData();
  } else {
    Serial.println("[DATA] Average retained because LoRa TX queue is full.");
  }
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
// CONTINUOUS INMP441 SAMPLING TASK
//
// The task continuously drains the INMP441 I2S DMA. A measurement
// contains exactly NOISE_SAMPLES_PER_WINDOW samples (16,000 Hz x 5 s
// = 80,000 samples). The main loop never performs the audio reads.
// =====================================================

void noiseSamplingTask(void *parameter) {
  static int32_t chunk[NOISE_CHUNK_SAMPLES];

  portENTER_CRITICAL(&noiseMux);
  noiseBuffers[0] = { 0.0, 0 };
  noiseBuffers[1] = { 0.0, 0 };
  noiseActiveBuffer = 0;
  noiseReadyBuffer = -1;
  portEXIT_CRITICAL(&noiseMux);

  while (noiseTaskRunning) {
    size_t bytesRead = 0;

    // Blocking is intentional here: ONLY this dedicated task waits for
    // I2S data. The main loop remains free for LVGL, touch and SCD41.
    esp_err_t result = i2s_read(
      I2S_PORT,
      chunk,
      sizeof(chunk),
      &bytesRead,
      portMAX_DELAY);

    if (result != ESP_OK || bytesRead == 0) continue;

    int sampleCount = bytesRead / sizeof(int32_t);
    int sourceIndex = 0;

    while (sourceIndex < sampleCount) {
      int active;
      uint32_t currentCount;

      portENTER_CRITICAL(&noiseMux);
      active = noiseActiveBuffer;
      currentCount = noiseBuffers[active].sampleCount;
      portEXIT_CRITICAL(&noiseMux);

      uint32_t remaining = NOISE_SAMPLES_PER_WINDOW - currentCount;
      int take = (int)min((uint32_t)(sampleCount - sourceIndex), remaining);

      portENTER_CRITICAL(&noiseMux);
      for (int i = 0; i < take; i++) {
        float sample = chunk[sourceIndex + i] >> 14;
        noiseBuffers[active].sumSquares +=
          (double)sample * (double)sample;
      }
      noiseBuffers[active].sampleCount += take;

      bool complete =
        (noiseBuffers[active].sampleCount >= NOISE_SAMPLES_PER_WINDOW);

      if (complete) {
        // Publish the completed buffer. The main loop will consume it.
        noiseReadyBuffer = active;

        // Switch immediately to the other buffer. This prevents samples
        // from the next 5-second period being mixed with the completed one.
        int nextBuffer = 1 - active;

        // If the main loop has not consumed the previous ready buffer,
        // the two buffers are both occupied. In normal operation the main
        // loop checks every 5 seconds, but protect against accidental
        // overwrite by waiting until a buffer is available.
        if (noiseReadyBuffer != nextBuffer) {
          noiseActiveBuffer = nextBuffer;
          noiseBuffers[nextBuffer].sumSquares = 0.0;
          noiseBuffers[nextBuffer].sampleCount = 0;
        }
      }
      portEXIT_CRITICAL(&noiseMux);

      sourceIndex += take;

      if (complete) {
        // If there are leftover samples in this DMA chunk, process them
        // as the first samples of the NEW 5-second window.
        continue;
      }
    }
  }

  vTaskDelete(nullptr);
}


// =====================================================
// FINALIZE COMPLETED 5-SECOND NOISE WINDOW
// =====================================================

// Copies one completed 80,000-sample window atomically and calculates
// one dB value from ONLY those samples.
bool finalizeNoiseReading() {
  double sumSquares = 0.0;
  uint32_t sampleCount = 0;
  int readyBuffer = -1;

  portENTER_CRITICAL(&noiseMux);
  readyBuffer = noiseReadyBuffer;
  if (readyBuffer >= 0) {
    sumSquares = noiseBuffers[readyBuffer].sumSquares;
    sampleCount = noiseBuffers[readyBuffer].sampleCount;

    noiseBuffers[readyBuffer].sumSquares = 0.0;
    noiseBuffers[readyBuffer].sampleCount = 0;
    noiseReadyBuffer = -1;
  }
  portEXIT_CRITICAL(&noiseMux);

  if (readyBuffer < 0 || sampleCount != NOISE_SAMPLES_PER_WINDOW) {
    Serial.print("Noise: 5-second window not ready. Samples: ");
    Serial.println(sampleCount);
    noiseHasData = false;
    return false;
  }

  float rms = sqrt(sumSquares / (double)sampleCount);
  if (rms < 1.0f) rms = 1.0f;

  float dB = NOISE_REF_DB + 20.0f * log10(rms / NOISE_REF_RMS);
  if (dB < 0.0f) dB = 0.0f;
  if (dB > 120.0f) dB = 120.0f;

  // No smoothing: this value comes directly from the completed
  // 5-second / 80,000-sample audio window.
  latestRMS = rms;
  latestNoise = dB;
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
  Serial.print("  |  Estimated dB (full 5-sec / 80,000 samples): ");
  Serial.print(latestNoise, 1);
  Serial.print("  |  Samples this window: ");
  Serial.println(sampleCount);

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

  uint16_t unusedCO2;
  float temp;
  float humidity;

  // SCD41 still provides temperature, but CO2 is no longer part of BANHA.
  error = scd4x.readMeasurement(unusedCO2, temp, humidity);

  if (error) {
    Serial.print("SCD41 Read Error: ");
    Serial.println(error);
    return false;
  }

  latestTemp = roundf(temp * 10.0f) / 10.0f;
  latestHumidity = humidity;
  scd41HasData = true;

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


  return true;
}


// =====================================================
// PRINT SENSOR DATA
// =====================================================

void printSensorData() {
  Serial.println("----------------------");
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
  // 0. SHARED SPI BUS MUTEX
  //
  // Created before Touch/LoRa init and before loraTask is
  // spawned, so both sides can safely rely on it existing.
  // ===================================================
  spiBusMutex = xSemaphoreCreateMutex();

  // ===================================================
  // 1. SCD41
  // ===================================================
  initSCD41();

  // ===================================================
  // 2. INMP441
  // ===================================================
  initINMP441();

  // Dedicated audio task: continuously drain INMP441 I2S DMA
  // independently from the main loop.
  xTaskCreatePinnedToCore(
    noiseSamplingTask,
    "NoiseSampler",
    4096,
    nullptr,
    2,
    &noiseTaskHandle,
    1);

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
  updateStatusLabels();

  if (objects.btn_start_rec != NULL) {
    lv_obj_add_event_cb(objects.btn_start_rec, startRecordingEvent, LV_EVENT_CLICKED, NULL);
  } else {
    Serial.println("ERROR: btn_start_rec not found.");
  }

  if (objects.btn_rec_stop_yes != NULL) {
    lv_obj_add_event_cb(objects.btn_rec_stop_yes, stopRecordingEvent, LV_EVENT_CLICKED, NULL);
  } else {
    Serial.println("ERROR: btn_rec_stop_yes not found.");
  }

  // ===================================================
  // 8. LORA INIT (still on the main task, one-time)
  //
  // Attaches to the already-running HSPI bus (touchSPI)
  // inside initLoRa(), independent of the TFT's VSPI bus.
  // ===================================================
  initLoRa();

  // ===================================================
  // 9. LORA TASK
  //
  // NEW: starts loraTask on the OPPOSITE core from the
  // Arduino loop() task (loop() runs on core 1 by default,
  // so loraTask is pinned to core 0). From this point on,
  // ALL LoRa I/O happens here, never inside loop().
  // ===================================================
  xTaskCreatePinnedToCore(
    loraTask,
    "LoRaTask",
    4096,
    nullptr,
    1,
    &loraTaskHandle,
    0);

  lastSensorCycle = millis();

  Serial.println();
  Serial.println("BANHA SYSTEM READY");
  Serial.println("CURRENT MODE: IDLE");
}


// =====================================================
// LOOP
//
// UPDATED: no longer touches LoRa at all - pollIncomingLoRa(),
// manageTxQueue() and manageNode2Heartbeat() have all moved
// into loraTask. loop() only drives LVGL/touch and the sensor
// cycle, so it can never be stalled by a LoRa transmission or
// retry storm again.
// =====================================================

void loop() {
  static uint32_t lastTick = millis();
  uint32_t now = millis();

  lv_tick_inc(now - lastTick);
  lastTick = now;

  // ---------------------------------------------------
  // UI
  // ---------------------------------------------------
  lv_timer_handler();
  ui_tick();

  // ---------------------------------------------------
  // Reflect current link/packet status (values are updated
  // by loraTask; loop() only reads them here for display).
  // ---------------------------------------------------
  updateStatusLabels();

  // ---------------------------------------------------
  // Every 5 seconds: finalize noise + read temperature + store
  // ---------------------------------------------------
  if (millis() - lastSensorCycle >= SENSOR_INTERVAL) {
    lastSensorCycle = millis();

    bool scdRead = readSCD41();
    bool noiseRead = finalizeNoiseReading();

    printSensorData();

    if (isRecording) {
      // Do not append beyond the 12-reading averaging window.
      // If transmission is temporarily blocked, the completed
      // average remains in RAM until enqueueData() succeeds.
      if (storedReadingCount < READINGS_PER_MINUTE) {
        if (scdRead && noiseRead) {
          storeSensorData();
        } else {
          Serial.println("Incomplete reading. Not stored.");
        }
      }

      if (storedReadingCount >= READINGS_PER_MINUTE) {
        calculateAndSendAverage();
      }
    }
  }

  // A tiny yield keeps the idle/watchdog housekeeping on this
  // core happy; LVGL itself is still serviced every iteration.
  vTaskDelay(pdMS_TO_TICKS(1));
}
