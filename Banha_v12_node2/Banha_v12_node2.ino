/*
   ============================================================
   BANHA LoRa Node 2 - v12 NON-BLOCKING SUPABASE QUEUE
   RECEIVER + SUPABASE UPLOADER + WIFI WEB CONFIG
   Compatible with BANHA Node 1 v11
   ============================================================

   WHAT CHANGED FROM THE PREVIOUS "UPDATED" VERSION
   ============================================================
   All three Supabase HTTP calls - createRecordingInDatabase(),
   uploadEnvironmentalData(), and stopRecordingInDatabase() -
   previously gave up permanently on the FIRST failed attempt.
   In practice this caused real, permanent data loss whenever
   the ESP32's HTTPS/TLS connection to Supabase hiccuped
   (commonly reported as HTTP Code: -1, a client-side
   connection/timeout failure, not a server response).

   FIX: a shared helper, supabaseRequest(), now retries up to
   3 times with a short backoff (400ms, 800ms) whenever the
   request fails for a TRANSIENT reason. A 409 (duplicate key
   - e.g. the same packet_number already exists) is treated as
   "handled, don't retry" rather than a failure, since retrying
   a genuine duplicate can't ever succeed and isn't a real
   problem - the data is already safely in the database.

   Also increased HTTPClient's connect/response timeout from
   the ~5s default to 8s, since ESP32 TLS handshakes to
   Supabase can occasionally take longer than that, especially
   later in a long-running session as heap fragments.

   IMPORTANT CAVEAT (unchanged from before, just documented):
   The LoRa ACK sent back to Node 1 confirms LoRa DELIVERY,
   not Supabase SUCCESS - sendAck() runs before
   handleStartCommand()/handleDataCommand()/handleStopCommand().
   These retries make a Supabase failure much less likely, but
   if all 3 attempts still fail, Node 1 will not know and will
   not retry, since it already received its ACK. This is a
   structural characteristic of the current protocol, not a bug
   introduced by this file - flagging it here for visibility.

   LoRa protocol (unchanged):

   START:
   NODE:1,TYPE:START,SEQ:1

   STOP:
   NODE:1,TYPE:STOP,SEQ:2

   DATA:
   NODE:1,TYPE:DATA,SEQ:3,PACKET:1,
   AVG_CO2:950.50,AVG_TEMP:26.40,AVG_NOISE:52.30

   ACK:
   NODE:2,TYPE:ACK,SEQ:1,ACKTYPE:START

   ============================================================
*/


#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <SPI.h>
#include <LoRa.h>
#include <stdlib.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>


// ============================================================
// ACCESS POINT
// ============================================================

const char* AP_SSID = "BANHA-SETUP";
const char* AP_PASSWORD = "banha@nbsc2026";


// ============================================================
// WEB SERVER
// ============================================================

WebServer server(80);


// ============================================================
// PREFERENCES
// ============================================================

Preferences preferences;


// ============================================================
// SAVED WIFI
// ============================================================

String savedWiFiSSID = "";
String savedWiFiPassword = "";


// ============================================================
// WIFI STATUS
// ============================================================

bool wifiConnected = false;
bool wifiConnectionAttempting = false;

unsigned long wifiConnectStartMillis = 0;
unsigned long lastWiFiReconnectAttempt = 0;

const unsigned long WIFI_CONNECT_TIMEOUT = 15000;
const unsigned long WIFI_RECONNECT_INTERVAL = 15000;


// ============================================================
// SUPABASE
// ============================================================

const char* SUPABASE_URL =
  "https://jeiolvlujtnmkwprppdu.supabase.co";

// ANON / PUBLISHABLE KEY
const char* SUPABASE_KEY =
  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImplaW9sdmx1anRubWt3cHJwcGR1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODc2NjQwNDksImV4cCI6MjEwMzI0MDA0OX0.I6vbxGR_mdU4p7hTxs9b2ASgBnd_5ltavPwlRuDdXr8";


// ============================================================
// DEVICE UUID
// ============================================================

const char* DEVICE_ID =
  "11111111-1111-1111-1111-111111111111";


// ============================================================
// LED
// ============================================================

#define LED_PIN 2


// ============================================================
// NODE 2 LoRa PINS
// ============================================================

#define LORA_SCK 23
#define LORA_MISO 19
#define LORA_MOSI 17

#define LORA_SS 16
#define LORA_RST 14
#define LORA_DIO0 26

#define LORA_BAND 433E6


// ============================================================
// LoRa RADIO SETTINGS
//
// MUST MATCH NODE 1
// ============================================================

#define LORA_SPREADING_FACTOR 7
#define LORA_BANDWIDTH 125E3
#define LORA_CODING_RATE 5


// ============================================================
// LoRa STATUS
// ============================================================

bool loraReady = false;


// ============================================================
// SUPABASE STATUS
// ============================================================

String lastSupabaseStatus = "Waiting";


// ============================================================
// SUPABASE RETRY SETTINGS
// ============================================================

const int SUPABASE_MAX_ATTEMPTS = 3;
const unsigned long SUPABASE_HTTP_TIMEOUT_MS = 8000;


// ============================================================
// RECORDING SESSION
// ============================================================

bool isRecordingActive = false;

String currentRecordingId = "";

unsigned long recordingStartMillis = 0;

unsigned long lastUploadedPacketNumber = 0;


// ============================================================
// RECEIVED DATA
// ============================================================

unsigned long receivedPacketNumber = 0;

float receivedAverageCO2 = 0.0f;
float receivedAverageTemp = 0.0f;
float receivedAverageNoise = 0.0f;


// ============================================================
// NON-BLOCKING SUPABASE DATA QUEUE
// ============================================================
// LoRa reception stays on the Arduino loop. Supabase uploads
// happen in a separate FreeRTOS task, so HTTPS/TLS timeouts do
// NOT stop Node 2 from receiving LoRa packets.
// ============================================================

struct PendingReading {
  char recordingId[40];
  uint32_t packetNumber;
  uint32_t seq;
  float co2;
  float temperature;
  float noise;
};

const int DATA_UPLOAD_QUEUE_SIZE = 60;
QueueHandle_t dataUploadQueue = nullptr;
TaskHandle_t dataUploaderTaskHandle = nullptr;

volatile uint32_t successfulUploadCount = 0;
volatile uint32_t failedUploadRetryCount = 0;
volatile bool uploaderBusy = false;

// One item is retained here while the uploader is retrying it.
// It is NEVER discarded just because Supabase is unavailable.
PendingReading retryReading;
bool retryReadingValid = false;
unsigned long nextRetryMillis = 0;
const unsigned long UPLOAD_RETRY_DELAY_MS = 5000;


// ============================================================
// SEQUENCE HISTORY
// ============================================================

#define SEQ_HISTORY_SIZE 20

uint32_t seqHistory[SEQ_HISTORY_SIZE];

int seqHistoryIndex = 0;

bool seqHistoryFull = false;


// ============================================================
// CHECK DUPLICATE SEQ
// ============================================================

bool isDuplicateSeq(uint32_t seq) {

  int count =
    seqHistoryFull
      ? SEQ_HISTORY_SIZE
      : seqHistoryIndex;

  for (int i = 0; i < count; i++) {

    if (seqHistory[i] == seq) {
      return true;
    }
  }

  return false;
}


// ============================================================
// MARK SEQ PROCESSED
// ============================================================

void markSeqProcessed(uint32_t seq) {

  seqHistory[seqHistoryIndex] = seq;

  seqHistoryIndex++;

  if (seqHistoryIndex >= SEQ_HISTORY_SIZE) {

    seqHistoryIndex = 0;

    seqHistoryFull = true;
  }
}


// ============================================================
// START ACCESS POINT
// ============================================================

void startAccessPoint() {

  Serial.println();
  Serial.println("Starting BANHA WiFi Setup AP...");

  WiFi.mode(WIFI_AP_STA);

  bool started = WiFi.softAP(
    AP_SSID,
    AP_PASSWORD);

  if (started) {

    Serial.println();
    Serial.println("================================");
    Serial.println("BANHA SETUP WIFI READY");
    Serial.print("SSID: ");
    Serial.println(AP_SSID);

    Serial.print("Password: ");
    Serial.println(AP_PASSWORD);

    Serial.print("Setup URL: http://");
    Serial.println(WiFi.softAPIP());

    Serial.println("================================");

  } else {

    Serial.println("Failed to start BANHA AP!");
  }
}


// ============================================================
// LOAD WIFI
// ============================================================

void loadSavedWiFi() {

  preferences.begin(
    "banha-wifi",
    true);

  savedWiFiSSID =
    preferences.getString(
      "ssid",
      "");

  savedWiFiPassword =
    preferences.getString(
      "password",
      "");

  preferences.end();

  Serial.println();
  Serial.println("Loading saved WiFi...");

  if (savedWiFiSSID.length() == 0) {

    Serial.println(
      "No saved WiFi configuration.");

    return;
  }

  Serial.print("Saved SSID: ");
  Serial.println(savedWiFiSSID);
}


// ============================================================
// SAVE WIFI
// ============================================================

void saveWiFi(
  String ssid,
  String password) {

  preferences.begin(
    "banha-wifi",
    false);

  preferences.putString(
    "ssid",
    ssid);

  preferences.putString(
    "password",
    password);

  preferences.end();

  savedWiFiSSID = ssid;
  savedWiFiPassword = password;

  Serial.println(
    "WiFi credentials saved.");
}


// ============================================================
// START WIFI CONNECTION
// ============================================================

void startWiFiConnection() {

  if (savedWiFiSSID.length() == 0) {

    Serial.println(
      "No WiFi configured.");

    wifiConnected = false;
    wifiConnectionAttempting = false;

    return;
  }

  if (WiFi.status() == WL_CONNECTED) {

    wifiConnected = true;
    wifiConnectionAttempting = false;

    return;
  }

  Serial.println();
  Serial.println(
    "Starting WiFi connection...");

  Serial.print("SSID: ");
  Serial.println(savedWiFiSSID);

  WiFi.mode(WIFI_AP_STA);

  WiFi.begin(
    savedWiFiSSID.c_str(),
    savedWiFiPassword.c_str());

  wifiConnectionAttempting = true;

  wifiConnectStartMillis = millis();

  lastWiFiReconnectAttempt = millis();
}


// ============================================================
// MAINTAIN WIFI
// ============================================================

void maintainWiFiConnection() {

  if (WiFi.status() == WL_CONNECTED) {

    if (!wifiConnected) {

      Serial.println();
      Serial.println("WiFi connected!");

      Serial.print("Router IP: ");
      Serial.println(WiFi.localIP());
    }

    wifiConnected = true;
    wifiConnectionAttempting = false;

    return;
  }

  wifiConnected = false;

  if (wifiConnectionAttempting) {

    if (
      millis() - wifiConnectStartMillis
      < WIFI_CONNECT_TIMEOUT) {
      return;
    }

    Serial.println();
    Serial.println(
      "WiFi connection attempt timed out.");

    WiFi.disconnect(
      false,
      false);

    wifiConnectionAttempting = false;

    lastWiFiReconnectAttempt = millis();

    return;
  }

  if (savedWiFiSSID.length() == 0) {
    return;
  }

  if (
    millis() - lastWiFiReconnectAttempt
    < WIFI_RECONNECT_INTERVAL) {
    return;
  }

  Serial.println();
  Serial.println(
    "Retrying configured WiFi...");

  startWiFiConnection();
}


// ============================================================
// ENSURE WIFI
// ============================================================

bool ensureWiFi() {

  if (WiFi.status() == WL_CONNECTED) {

    wifiConnected = true;

    return true;
  }

  wifiConnected = false;

  if (!wifiConnectionAttempting) {

    if (
      savedWiFiSSID.length() > 0 && millis() - lastWiFiReconnectAttempt >= WIFI_RECONNECT_INTERVAL) {

      startWiFiConnection();
    }
  }

  return false;
}


// ============================================================
// HTML STYLE
// ============================================================

String getSharedStyle() {

  String css =
    "<style>"
    ":root{"
    "--bg:#f1f5f9;"
    "--card:#ffffff;"
    "--text:#1e293b;"
    "--muted:#64748b;"
    "--border:#e2e8f0;"
    "--primary:#15803d;"
    "--primary-dark:#0f5c2c;"
    "--danger:#dc2626;"
    "--warn:#d97706;"
    "--radius:16px;"
    "}"
    "*{box-sizing:border-box;}"
    "body{"
    "margin:0;"
    "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial,sans-serif;"
    "background:var(--bg);"
    "color:var(--text);"
    "-webkit-font-smoothing:antialiased;"
    "}"
    ".container{"
    "max-width:640px;"
    "margin:0 auto;"
    "padding:24px 18px 40px;"
    "}"
    ".topbar{"
    "text-align:center;"
    "padding:8px 0 22px;"
    "}"
    ".topbar .logo{"
    "display:inline-flex;"
    "align-items:center;"
    "justify-content:center;"
    "width:52px;"
    "height:52px;"
    "border-radius:14px;"
    "background:linear-gradient(135deg,#22c55e,var(--primary-dark));"
    "color:white;"
    "font-size:22px;"
    "font-weight:800;"
    "margin-bottom:10px;"
    "}"
    ".topbar h1{"
    "margin:0;"
    "font-size:20px;"
    "}"
    ".topbar p{"
    "margin:4px 0 0;"
    "font-size:13px;"
    "color:var(--muted);"
    "}"
    ".card{"
    "background:var(--card);"
    "padding:22px;"
    "border-radius:var(--radius);"
    "margin-bottom:18px;"
    "border:1px solid var(--border);"
    "}"
    ".status-row{"
    "display:flex;"
    "align-items:center;"
    "justify-content:space-between;"
    "gap:10px;"
    "padding:11px 0;"
    "border-bottom:1px solid var(--border);"
    "}"
    ".status-row:last-child{"
    "border-bottom:none;"
    "}"
    ".status-label{"
    "font-size:13px;"
    "color:var(--muted);"
    "font-weight:600;"
    "}"
    ".status-value{"
    "font-size:14px;"
    "font-weight:600;"
    "text-align:right;"
    "word-break:break-word;"
    "}"
    ".badge{"
    "display:inline-flex;"
    "align-items:center;"
    "gap:6px;"
    "font-size:13px;"
    "font-weight:700;"
    "padding:4px 10px;"
    "border-radius:999px;"
    "}"
    ".badge.ok{"
    "background:#dcfce7;"
    "color:#15803d;"
    "}"
    ".badge.bad{"
    "background:#fee2e2;"
    "color:#dc2626;"
    "}"
    ".badge.warn{"
    "background:#fef3c7;"
    "color:#b45309;"
    "}"
    ".badge .dot{"
    "width:7px;"
    "height:7px;"
    "border-radius:50%;"
    "background:currentColor;"
    "}"
    "label{"
    "display:block;"
    "margin-top:16px;"
    "font-weight:700;"
    "font-size:13px;"
    "color:var(--muted);"
    "}"
    "input{"
    "width:100%;"
    "padding:13px 14px;"
    "margin-top:7px;"
    "border:1.5px solid var(--border);"
    "border-radius:10px;"
    "font-size:16px;"
    "background:#f8fafc;"
    "}"
    "button,.button{"
    "display:block;"
    "width:100%;"
    "padding:14px;"
    "margin-top:22px;"
    "background:var(--primary);"
    "color:white;"
    "border:none;"
    "border-radius:10px;"
    "font-size:16px;"
    "font-weight:700;"
    "cursor:pointer;"
    "text-align:center;"
    "text-decoration:none;"
    "}"
    ".small{"
    "font-size:12.5px;"
    "color:var(--muted);"
    "line-height:1.5;"
    "}"
    "</style>";

  return css;
}


// ============================================================
// DASHBOARD HTML
// ============================================================

String getDashboardHTML() {

  bool routerConnected =
    (WiFi.status() == WL_CONNECTED);

  String wifiBadge;

  if (routerConnected) {

    wifiBadge =
      "<span class='badge ok'>"
      "<span class='dot'></span>Connected"
      "</span>";

  } else if (wifiConnectionAttempting) {

    wifiBadge =
      "<span class='badge warn'>"
      "<span class='dot'></span>Connecting..."
      "</span>";

  } else {

    wifiBadge =
      "<span class='badge bad'>"
      "<span class='dot'></span>Disconnected"
      "</span>";
  }


  String loraBadge;

  if (loraReady) {

    loraBadge =
      "<span class='badge ok'>"
      "<span class='dot'></span>Ready"
      "</span>";

  } else {

    loraBadge =
      "<span class='badge bad'>"
      "<span class='dot'></span>Not Ready"
      "</span>";
  }


  String recordingBadge;

  if (isRecordingActive) {

    recordingBadge =
      "<span class='badge warn'>"
      "<span class='dot'></span>Recording"
      "</span>";

  } else {

    recordingBadge =
      "<span class='badge ok'>"
      "<span class='dot'></span>Idle"
      "</span>";
  }


  String html =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<meta charset='UTF-8'>"
    "<title>BANHA Node 2</title>"
    + getSharedStyle() + "</head>"
                         "<body>"
                         "<div class='container'>"

                         "<div class='topbar'>"
                         "<div class='logo'>B2</div>"
                         "<h1>BANHA Node 2</h1>"
                         "<p>LoRa Receiver &amp; WiFi Configuration</p>"
                         "</div>"

                         "<div class='card'>"

                         "<h2>System Status</h2>"

                         "<div class='status-row'>"
                         "<span class='status-label'>Router WiFi</span>"
                         "<span class='status-value'>"
    + wifiBadge + "</span>"
                  "</div>"

                  "<div class='status-row'>"
                  "<span class='status-label'>Configured Network</span>"
                  "<span class='status-value'>"
    + (savedWiFiSSID.length() > 0
         ? savedWiFiSSID
         : String("None"))
    + "</span>"
      "</div>"

      "<div class='status-row'>"
      "<span class='status-label'>Connected SSID</span>"
      "<span class='status-value'>"
    + (routerConnected
         ? WiFi.SSID()
         : String("&mdash;"))
    + "</span>"
      "</div>"

      "<div class='status-row'>"
      "<span class='status-label'>Router IP</span>"
      "<span class='status-value'>"
    + (routerConnected
         ? WiFi.localIP().toString()
         : String("&mdash;"))
    + "</span>"
      "</div>"

      "<div class='status-row'>"
      "<span class='status-label'>Setup Hotspot</span>"
      "<span class='status-value'>"
    + String(AP_SSID) + " &middot; " + WiFi.softAPIP().toString() + "</span>"
                                                                    "</div>"

                                                                    "<div class='status-row'>"
                                                                    "<span class='status-label'>LoRa Radio</span>"
                                                                    "<span class='status-value'>"
    + loraBadge + "</span>"
                  "</div>"

                  "<div class='status-row'>"
                  "<span class='status-label'>Recording</span>"
                  "<span class='status-value'>"
    + recordingBadge + "</span>"
                       "</div>"

                       "</div>"


                       "<div class='card'>"
                       "<h2>Change WiFi Configuration</h2>"

                       "<form action='/save' method='POST'>"

                       "<label>WiFi Name (SSID)</label>"
                       "<input type='text' name='ssid' "
                       "autocomplete='off' value='"
    + savedWiFiSSID + "' required>"

                      "<label>WiFi Password</label>"
                      "<input type='password' name='password' "
                      "autocomplete='new-password'>"

                      "<button type='submit'>Save and Connect</button>"

                      "</form>"
                      "</div>"


                      "<div class='card'>"
                      "<h2>LoRa Configuration</h2>"

                      "<div class='status-row'>"
                      "<span class='status-label'>Frequency</span>"
                      "<span class='status-value'>433 MHz</span>"
                      "</div>"

                      "<div class='status-row'>"
                      "<span class='status-label'>Spreading Factor</span>"
                      "<span class='status-value'>SF7</span>"
                      "</div>"

                      "<div class='status-row'>"
                      "<span class='status-label'>Bandwidth</span>"
                      "<span class='status-value'>125 kHz</span>"
                      "</div>"

                      "<div class='status-row'>"
                      "<span class='status-label'>Coding Rate</span>"
                      "<span class='status-value'>4/5</span>"
                      "</div>"

                      "</div>"


                      "<div class='card'>"
                      "<h2>Setup</h2>"
                      "<p class='small'>"
                      "Connect your phone or computer to "
                      "<b>BANHA-SETUP</b>, then open "
                      "<b>192.168.4.1</b>."
                      "</p>"
                      "</div>"

                      "</div>"
                      "</body>"
                      "</html>";

  return html;
}


// ============================================================
// WEB ROOT
// ============================================================

void handleRoot() {

  server.send(
    200,
    "text/html",
    getDashboardHTML());
}


// ============================================================
// WEB SAVE WIFI
// ============================================================

void handleSaveWiFi() {

  if (!server.hasArg("ssid")) {

    server.send(
      400,
      "text/plain",
      "SSID is required.");

    return;
  }

  String newSSID =
    server.arg("ssid");

  String newPassword =
    server.arg("password");

  newSSID.trim();

  if (newSSID.length() == 0) {

    server.send(
      400,
      "text/plain",
      "SSID cannot be empty.");

    return;
  }

  saveWiFi(
    newSSID,
    newPassword);

  server.send(
    200,
    "text/html",
    "<html><body>"
    "<h2>WiFi saved.</h2>"
    "<p>Connecting...</p>"
    "<a href='/'>Return</a>"
    "</body></html>");

  wifiConnectionAttempting = false;

  WiFi.disconnect(
    false,
    false);

  lastWiFiReconnectAttempt = 0;

  startWiFiConnection();
}


// ============================================================
// WEB STATUS
// ============================================================

void handleStatus() {

  String json = "{";

  json +=
    "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");

  json +=
    ",\"wifi_connecting\":" + String(wifiConnectionAttempting ? "true" : "false");

  json +=
    ",\"configured_ssid\":\"" + savedWiFiSSID + "\"";

  json +=
    ",\"ssid\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("")) + "\"";

  json +=
    ",\"ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("")) + "\"";

  json +=
    ",\"ap_ssid\":\"" + String(AP_SSID) + "\"";

  json +=
    ",\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\"";

  json +=
    ",\"lora_ready\":" + String(loraReady ? "true" : "false");

  json +=
    ",\"recording_active\":" + String(isRecordingActive ? "true" : "false");

  json +=
    ",\"pending_uploads\":" + String(getPendingUploadCount());

  json +=
    ",\"uploader_busy\":" + String(uploaderBusy ? "true" : "false");

  json +=
    ",\"successful_uploads\":" + String((uint32_t)successfulUploadCount);

  json +=
    ",\"failed_upload_retries\":" + String((uint32_t)failedUploadRetryCount);

  json += "}";

  server.send(
    200,
    "application/json",
    json);
}


// ============================================================
// START WEB SERVER
// ============================================================

void startWebServer() {

  server.on(
    "/",
    HTTP_GET,
    handleRoot);

  server.on(
    "/save",
    HTTP_POST,
    handleSaveWiFi);

  server.on(
    "/status",
    HTTP_GET,
    handleStatus);

  server.begin();

  Serial.println(
    "Web server started.");
}


// ============================================================
// GET PACKET VALUE
// ============================================================

String getPacketValue(
  const String& message,
  const String& key) {

  int startIndex =
    message.indexOf(key);

  if (startIndex == -1) {
    return "";
  }

  startIndex += key.length();

  int endIndex =
    message.indexOf(
      ",",
      startIndex);

  if (endIndex == -1) {
    endIndex = message.length();
  }

  return message.substring(
    startIndex,
    endIndex);
}


// ============================================================
// SEND ACK
// ============================================================

bool sendAck(
  uint32_t seq,
  const String& ackType) {

  if (!loraReady) {
    return false;
  }

  String message =
    "NODE:2,TYPE:ACK,SEQ:" + String(seq) + ",ACKTYPE:" + ackType;

  Serial.println();
  Serial.println("--------------------------------");
  Serial.println("LORA ACK TRANSMITTING");
  Serial.println(message);
  Serial.println("--------------------------------");

  // Stop receive mode before TX.
  LoRa.idle();

  int result;

  result = LoRa.beginPacket();

  if (result == 0) {

    Serial.println(
      "ERROR: LoRa beginPacket() failed.");

    LoRa.receive();

    return false;
  }

  LoRa.print(message);

  result = LoRa.endPacket();

  if (result == 0) {

    Serial.println(
      "ERROR: LoRa endPacket() failed.");

    LoRa.receive();

    return false;
  }

  Serial.println(
    "ACK SENT SUCCESSFULLY.");

  // Immediately return to receive mode.
  LoRa.receive();

  return true;
}


// ============================================================
// EXTRACT RECORDING UUID
// ============================================================

String extractRecordingId(
  const String& response) {

  int idKey =
    response.indexOf("\"id\":\"");

  if (idKey == -1) {
    return "";
  }

  idKey += 6;

  int endQuote =
    response.indexOf(
      "\"",
      idKey);

  if (endQuote == -1) {
    return "";
  }

  return response.substring(
    idKey,
    endQuote);
}


// ============================================================
// SUPABASE REQUEST WITH RETRY
// ============================================================
// IMPORTANT:
// This function is allowed to block because it runs ONLY inside
// the Supabase uploader task. The main Arduino loop remains free
// to receive LoRa packets and serve the web page.
// ============================================================

bool supabaseRequest(
  const String& method,
  const String& endpoint,
  const String& payload,
  bool includePreferHeader,
  int& outHttpCode,
  String& outResponse) {

  outHttpCode = -1;
  outResponse = "";

  for (int attempt = 1; attempt <= SUPABASE_MAX_ATTEMPTS; attempt++) {

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[Supabase] WiFi not connected.");
      outHttpCode = -1;
      outResponse = "WiFi unavailable";
      return false;
    }

    HTTPClient http;
    http.setConnectTimeout(SUPABASE_HTTP_TIMEOUT_MS);
    http.setTimeout(SUPABASE_HTTP_TIMEOUT_MS);

    if (!http.begin(endpoint)) {
      Serial.println("[Supabase] HTTP begin failed.");
      outHttpCode = -1;
      outResponse = "HTTP begin failed";
    } else {

      http.addHeader("Content-Type", "application/json");
      http.addHeader("apikey", SUPABASE_KEY);
      http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

      if (includePreferHeader) {
        http.addHeader("Prefer", "return=representation");
      }

      int httpCode;

      if (method == "POST") {
        httpCode = http.POST(payload);
      } else {
        httpCode = http.sendRequest(method.c_str(), payload);
      }

      String response = http.getString();
      http.end();

      outHttpCode = httpCode;
      outResponse = response;

      Serial.print("[Supabase attempt ");
      Serial.print(attempt);
      Serial.print("/");
      Serial.print(SUPABASE_MAX_ATTEMPTS);
      Serial.print("] HTTP Code: ");
      Serial.println(httpCode);

      if (response.length() > 0) {
        Serial.print("Response: ");
        Serial.println(response);
      }

      if (httpCode >= 200 && httpCode < 300) {
        return true;
      }

      // 409 means the row already exists. Treat it as success.
      if (httpCode == 409) {
        Serial.println("[Supabase] 409 duplicate - already stored.");
        return true;
      }
    }

    if (attempt < SUPABASE_MAX_ATTEMPTS) {
      unsigned long backoff = 400UL * attempt;
      Serial.print("[Supabase] Retry in ");
      Serial.print(backoff);
      Serial.println(" ms...");
      delay(backoff);
    }
  }

  Serial.println("[Supabase] All retry attempts exhausted.");
  return false;
}


// ============================================================
// CREATE RECORDING
// ============================================================

bool createRecordingInDatabase() {

  if (!ensureWiFi()) {
    Serial.println("Cannot create recording: WiFi unavailable.");
    lastSupabaseStatus = "WiFi unavailable";
    return false;
  }

  Serial.println();
  Serial.println("SUPABASE: CREATING RECORDING...");
  lastSupabaseStatus = "Creating recording";

  String endpoint = String(SUPABASE_URL) + "/rest/v1/recordings";
  String payload = "{";
  payload += "\"device_id\":\"";
  payload += DEVICE_ID;
  payload += "\",\"status\":\"recording\"";
  payload += "}";

  int httpCode;
  String response;

  bool ok = supabaseRequest(
    "POST", endpoint, payload, true, httpCode, response);

  if (!ok) {
    lastSupabaseStatus = "Create failed: HTTP " + String(httpCode);
    return false;
  }

  currentRecordingId = extractRecordingId(response);

  if (currentRecordingId.length() == 0) {
    lastSupabaseStatus = "UUID not found";
    Serial.println("ERROR: Supabase did not return recording UUID.");
    return false;
  }

  lastSupabaseStatus = "Recording created";

  Serial.print("Recording UUID: ");
  Serial.println(currentRecordingId);
  return true;
}


// ============================================================
// BUILD DATA UPLOAD PAYLOAD
// ============================================================

bool uploadEnvironmentalData(const PendingReading& item) {

  if (item.recordingId[0] == '\0') {
    Serial.println("[UPLOAD] Missing recording UUID.");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[UPLOAD] WiFi unavailable. Keeping reading queued.");
    return false;
  }

  String endpoint = String(SUPABASE_URL) + "/rest/v1/environmental_readings";

  String payload = "{";
  payload += "\"recording_id\":\"";
  payload += item.recordingId;
  payload += "\",\"packet_number\":";
  payload += String(item.packetNumber);
  payload += ",\"average_co2\":";
  payload += String(item.co2, 2);
  payload += ",\"average_temperature\":";
  payload += String(item.temperature, 2);
  payload += ",\"average_noise\":";
  payload += String(item.noise, 2);
  payload += "}";

  Serial.println();
  Serial.println("SUPABASE: UPLOADING QUEUED DATA");
  Serial.print("Recording: ");
  Serial.println(item.recordingId);
  Serial.print("Packet: ");
  Serial.println(item.packetNumber);
  Serial.print("SEQ: ");
  Serial.println(item.seq);
  Serial.println(payload);

  int httpCode;
  String response;

  bool ok = supabaseRequest(
    "POST", endpoint, payload, false, httpCode, response);

  if (ok) {
    Serial.println("SUPABASE: READING STORED (or already existed).");
    return true;
  }

  Serial.print("SUPABASE: READING FAILED, HTTP ");
  Serial.println(httpCode);
  return false;
}


// ============================================================
// STOP RECORDING IN DATABASE
// ============================================================

bool stopRecordingInDatabase() {

  if (currentRecordingId.length() == 0) {
    Serial.println("No recording UUID available.");
    return false;
  }

  if (!ensureWiFi()) {
    lastSupabaseStatus = "Stop failed: WiFi unavailable";
    return false;
  }

  lastSupabaseStatus = "Stopping recording";

  unsigned long elapsedMilliseconds = millis() - recordingStartMillis;
  unsigned long durationSeconds = elapsedMilliseconds / 1000;

  Serial.print("Duration seconds: ");
  Serial.println(durationSeconds);

  String endpoint = String(SUPABASE_URL) + "/rest/v1/recordings?id=eq." + currentRecordingId;

  String payload = "{";
  payload += "\"duration_seconds\":";
  payload += String(durationSeconds);
  payload += ",\"status\":\"pending_assessment\"";
  payload += "}";

  int httpCode;
  String response;

  bool ok = supabaseRequest(
    "PATCH", endpoint, payload, true, httpCode, response);

  if (ok) {
    lastSupabaseStatus = "Recording stopped";
    Serial.println("SUPABASE: RECORDING STOPPED!");
    return true;
  }

  lastSupabaseStatus = "Stop failed: HTTP " + String(httpCode);
  Serial.println("SUPABASE: STOP FAILED after retries!");
  return false;
}


// ============================================================
// QUEUE HELPERS
// ============================================================

int getPendingUploadCount() {
  if (dataUploadQueue == nullptr) {
    return 0;
  }
  return (int)uxQueueMessagesWaiting(dataUploadQueue) + (retryReadingValid ? 1 : 0);
}


bool isReadingAlreadyQueued(
  const char* recordingId,
  uint32_t packetNumber) {

  if (retryReadingValid && strcmp(retryReading.recordingId, recordingId) == 0 && retryReading.packetNumber == packetNumber) {
    return true;
  }

  if (dataUploadQueue == nullptr) {
    return false;
  }

  // FreeRTOS queues do not provide a peek-all operation. We use
  // the SEQ history as the primary duplicate guard after enqueue,
  // while the database's packet_number uniqueness protects against
  // a rare retransmission with a different SEQ.
  return false;
}


bool enqueueEnvironmentalReading(
  const char* recordingId,
  uint32_t packetNumber,
  uint32_t seq,
  float co2,
  float temperature,
  float noise) {

  if (dataUploadQueue == nullptr) {
    Serial.println("[QUEUE] Queue not initialized.");
    return false;
  }

  if (packetNumber == 0 || recordingId == nullptr || recordingId[0] == '\0') {
    Serial.println("[QUEUE] Invalid reading.");
    return false;
  }

  if (!isfinite(co2) || !isfinite(temperature) || !isfinite(noise)) {
    Serial.println("[QUEUE] Invalid sensor value.");
    return false;
  }

  if (isReadingAlreadyQueued(recordingId, packetNumber)) {
    Serial.println("[QUEUE] Reading already retained for retry.");
    return true;
  }

  PendingReading item{};
  strncpy(item.recordingId, recordingId, sizeof(item.recordingId) - 1);
  item.recordingId[sizeof(item.recordingId) - 1] = '\0';
  item.packetNumber = packetNumber;
  item.seq = seq;
  item.co2 = co2;
  item.temperature = temperature;
  item.noise = noise;

  if (xQueueSend(dataUploadQueue, &item, 0) != pdTRUE) {
    Serial.println("[QUEUE FULL] DATA NOT ACKED. Node 1 must retry.");
    return false;
  }

  Serial.print("[QUEUE] DATA accepted. Pending uploads: ");
  Serial.println(getPendingUploadCount());
  return true;
}


// ============================================================
// SUPABASE DATA UPLOADER TASK
// ============================================================

void supabaseDataUploaderTask(void* parameter) {

  PendingReading item;

  for (;;) {

    // If a failed item is retained, retry it after the delay.
    if (retryReadingValid) {

      if ((long)(millis() - nextRetryMillis) < 0) {
        vTaskDelay(pdMS_TO_TICKS(250));
        continue;
      }

      // If WiFi is down, do not start an HTTPS request.
      if (WiFi.status() != WL_CONNECTED) {
        nextRetryMillis = millis() + UPLOAD_RETRY_DELAY_MS;
        failedUploadRetryCount++;
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }

      uploaderBusy = true;

      bool ok = uploadEnvironmentalData(retryReading);

      uploaderBusy = false;

      if (ok) {
        successfulUploadCount++;
        retryReadingValid = false;
        lastSupabaseStatus = "Queued reading uploaded";
        Serial.print("[QUEUE] Retry succeeded. Pending uploads: ");
        Serial.println(getPendingUploadCount());
      } else {
        failedUploadRetryCount++;
        nextRetryMillis = millis() + UPLOAD_RETRY_DELAY_MS;
        Serial.println("[QUEUE] Keeping failed reading. It will be retried.");
        vTaskDelay(pdMS_TO_TICKS(500));
      }

      continue;
    }

    // Wait for a new reading. This does not block the Arduino
    // loop because this is a separate FreeRTOS task.
    if (xQueueReceive(dataUploadQueue, &item, pdMS_TO_TICKS(1000)) == pdTRUE) {

      uploaderBusy = true;

      bool ok = uploadEnvironmentalData(item);

      uploaderBusy = false;

      if (ok) {
        successfulUploadCount++;
        lastSupabaseStatus = "Reading uploaded";
        Serial.print("[QUEUE] Upload success. Pending uploads: ");
        Serial.println(getPendingUploadCount());
      } else {
        // CRITICAL: DO NOT DROP THE ITEM.
        retryReading = item;
        retryReadingValid = true;
        nextRetryMillis = millis() + UPLOAD_RETRY_DELAY_MS;
        failedUploadRetryCount++;

        Serial.println("[QUEUE] Upload failed.");
        Serial.println("[QUEUE] READING RETAINED FOR RETRY.");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}


void startDataUploaderTask() {

  dataUploadQueue = xQueueCreate(
    DATA_UPLOAD_QUEUE_SIZE,
    sizeof(PendingReading));

  if (dataUploadQueue == nullptr) {
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.println("ERROR: Could not create DATA upload queue!");
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    return;
  }

  BaseType_t result = xTaskCreatePinnedToCore(
    supabaseDataUploaderTask,
    "SupabaseUploader",
    12288,
    nullptr,
    1,
    &dataUploaderTaskHandle,
    0);  // Core 0: WiFi/Supabase worker; LoRa loop stays on Arduino core.

  if (result != pdPASS) {
    Serial.println("ERROR: Could not start Supabase uploader task!");
    vQueueDelete(dataUploadQueue);
    dataUploadQueue = nullptr;
    return;
  }

  Serial.print("DATA upload queue ready: ");
  Serial.print(DATA_UPLOAD_QUEUE_SIZE);
  Serial.println(" readings.");
  Serial.println("Supabase uploader task started.");
}


// ============================================================
// HANDLE START
// ============================================================

void handleStartCommand(bool isDuplicate) {

  Serial.println();
  Serial.println("***** START COMMAND *****");

  if (isDuplicate) {
    Serial.println("[DUP] START already processed.");
    return;
  }

  if (isRecordingActive) {
    Serial.println("START ignored: recording already active.");
    return;
  }

  bool created = createRecordingInDatabase();

  if (!created) {
    Serial.println("Database recording creation failed.");
    return;
  }

  isRecordingActive = true;
  recordingStartMillis = millis();
  lastUploadedPacketNumber = 0;

  Serial.println();
  Serial.println("================================");
  Serial.println("RECORDING SESSION STARTED");
  Serial.print("Recording UUID: ");
  Serial.println(currentRecordingId);
  Serial.println("================================");
}


// ============================================================
// HANDLE STOP
// ============================================================

void handleStopCommand(bool isDuplicate) {

  Serial.println();
  Serial.println("***** STOP COMMAND *****");

  if (isDuplicate) {
    Serial.println("[DUP] STOP already processed.");
    return;
  }

  if (!isRecordingActive) {
    Serial.println("No active recording.");
    return;
  }

  // IMPORTANT:
  // We do not clear the session until the DB PATCH succeeds.
  // Also, queued DATA for this recording is retained separately
  // by the uploader task and will continue uploading.
  bool stopped = stopRecordingInDatabase();

  if (!stopped) {
    Serial.println("Database stop failed. Session state retained.");
    return;
  }

  isRecordingActive = false;

  Serial.println("RECORDING SESSION STOPPED");

  currentRecordingId = "";
  lastUploadedPacketNumber = 0;
}


// ============================================================
// PARSE DATA
// ============================================================

bool parseDataPacket(
  const String& message,
  uint32_t& packetNumber,
  float& averageCO2,
  float& averageTemp,
  float& averageNoise) {

  String packetValue = getPacketValue(message, "PACKET:");
  String averageCO2Value = getPacketValue(message, "AVG_CO2:");
  String averageTempValue = getPacketValue(message, "AVG_TEMP:");
  String averageNoiseValue = getPacketValue(message, "AVG_NOISE:");

  if (packetValue.length() == 0 || averageCO2Value.length() == 0 || averageTempValue.length() == 0 || averageNoiseValue.length() == 0) {
    return false;
  }

  char* endPtr = nullptr;
  unsigned long packet = strtoul(packetValue.c_str(), &endPtr, 10);

  if (endPtr == packetValue.c_str() || *endPtr != '\0' || packet == 0) {
    return false;
  }

  float co2 = averageCO2Value.toFloat();
  float temp = averageTempValue.toFloat();
  float noise = averageNoiseValue.toFloat();

  if (!isfinite(co2) || !isfinite(temp) || !isfinite(noise)) {
    return false;
  }

  packetNumber = (uint32_t)packet;
  averageCO2 = co2;
  averageTemp = temp;
  averageNoise = noise;

  return true;
}


// ============================================================
// HANDLE DATA
// ============================================================
// DATA is now accepted into RAM immediately. The actual HTTPS
// upload is performed by the FreeRTOS uploader task.
// ACK means: "Node 2 accepted and retained this reading", not
// "Supabase finished uploading it".
// ============================================================

bool handleDataCommand(
  const String& message,
  uint32_t seq) {

  if (!isRecordingActive) {
    Serial.println("DATA ignored: No active recording.");
    return false;
  }

  uint32_t packetNumber;
  float averageCO2;
  float averageTemp;
  float averageNoise;

  if (!parseDataPacket(
        message,
        packetNumber,
        averageCO2,
        averageTemp,
        averageNoise)) {

    Serial.println("Invalid DATA packet. No ACK sent.");
    return false;
  }

  Serial.println();
  Serial.println("***** DATA COMMAND *****");
  Serial.print("PACKET: ");
  Serial.println(packetNumber);
  Serial.print("SEQ: ");
  Serial.println(seq);
  Serial.print("CO2: ");
  Serial.println(averageCO2, 2);
  Serial.print("Temperature: ");
  Serial.println(averageTemp, 2);
  Serial.print("Noise: ");
  Serial.println(averageNoise, 2);

  // A duplicate SEQ means Node 2 already accepted this packet.
  // The caller will send the ACK again, but will not enqueue twice.
  if (isDuplicateSeq(seq)) {
    Serial.println("[DUP] SEQ already accepted. Upload not duplicated.");
    return true;
  }

  // The DB should normally reject duplicate packet_number values
  // with 409. We also reject obviously old packets here only when
  // they are already known to have been successfully uploaded.
  // Do NOT use lastUploadedPacketNumber as an ordering gate.
  // Packets can be retried and arrive later.

  bool queued = enqueueEnvironmentalReading(
    currentRecordingId.c_str(),
    packetNumber,
    seq,
    averageCO2,
    averageTemp,
    averageNoise);

  if (!queued) {
    Serial.println("[DATA] Could not retain reading. No ACK sent.");
    return false;
  }

  // Only mark SEQ after the reading is safely retained.
  markSeqProcessed(seq);

  receivedPacketNumber = packetNumber;
  receivedAverageCO2 = averageCO2;
  receivedAverageTemp = averageTemp;
  receivedAverageNoise = averageNoise;

  if (lastUploadedPacketNumber == 0 || packetNumber > lastUploadedPacketNumber) {
    // This is informational only. It is NOT used to reject packets.
    lastUploadedPacketNumber = packetNumber;
  }

  Serial.print("[DATA ACCEPTED] Pending uploads: ");
  Serial.println(getPendingUploadCount());

  return true;
}


// ============================================================
// HANDLE LoRa MESSAGE
// ============================================================

void handleLoRaMessage(const String& message) {

  Serial.println();
  Serial.println("================================");
  Serial.println("PROCESSING LoRa MESSAGE");
  Serial.print("Message: ");
  Serial.println(message);

  if (!message.startsWith("NODE:1,")) {
    Serial.println("Rejected: not from Node 1.");
    Serial.println("================================");
    return;
  }

  String packetType = getPacketValue(message, "TYPE:");

  if (packetType != "START" && packetType != "STOP" && packetType != "DATA") {
    Serial.println("Unknown TYPE. No ACK sent.");
    Serial.println("================================");
    return;
  }

  Serial.print("TYPE: ");
  Serial.println(packetType);

  String seqStr = getPacketValue(message, "SEQ:");

  if (seqStr.length() == 0) {
    Serial.println("[WARN] No SEQ field. Processing without ACK.");

    if (packetType == "START") {
      handleStartCommand(false);
    } else if (packetType == "STOP") {
      handleStopCommand(false);
    } else {
      Serial.println("DATA without SEQ is rejected for reliability.");
    }

    LoRa.receive();
    return;
  }

  char* endPtr = nullptr;
  unsigned long seqLong = strtoul(seqStr.c_str(), &endPtr, 10);

  if (endPtr == seqStr.c_str() || *endPtr != '\0') {
    Serial.println("Invalid SEQ. No ACK sent.");
    LoRa.receive();
    return;
  }

  uint32_t seq = (uint32_t)seqLong;

  Serial.print("SEQ: ");
  Serial.println(seq);

  bool duplicate = isDuplicateSeq(seq);

  if (duplicate) {
    Serial.println("[DUP] SEQ already accepted.");
  } else {
    Serial.println("[NEW] New SEQ.");
  }

  // ----------------------------------------------------------
  // DATA MUST BE VALIDATED / RETAINED BEFORE ACK.
  // If the RAM queue is full, do NOT ACK. Node 1 can retry.
  // ----------------------------------------------------------
  bool accepted = true;

  if (packetType == "DATA" && !duplicate) {
    accepted = handleDataCommand(message, seq);

    if (!accepted) {
      Serial.println("DATA was NOT accepted. ACK intentionally withheld.");
      LoRa.receive();
      return;
    }
  }

  // START/STOP retain the existing transport ACK behavior because
  // their database operation can take longer than Node 1's ACK
  // timeout. DATA uses the stronger queue-before-ACK rule above.
  bool ackSent = sendAck(seq, packetType);

  if (!ackSent) {
    Serial.println("WARNING: ACK failed.");
  }

  if (packetType == "START") {
    if (!duplicate) {
      // Mark before the DB call because START has historically
      // used transport ACK semantics. A retransmitted START after
      // the ACK should not create a second recording.
      markSeqProcessed(seq);
      handleStartCommand(false);
    }
  } else if (packetType == "STOP") {
    if (!duplicate) {
      markSeqProcessed(seq);
      handleStopCommand(false);
    }
  }
  // DATA has already been handled and its SEQ marked only after
  // successful queue insertion.

  LoRa.receive();

  Serial.print("LoRa RX MODE: READY | Pending uploads: ");
  Serial.println(getPendingUploadCount());
  Serial.println("================================");
}


// ============================================================
// INITIALIZE LoRa
// ============================================================

bool initializeLoRa() {

  Serial.println();
  Serial.println(
    "Starting LoRa...");

  Serial.println(
    "LoRa pins:");

  Serial.print(
    "  SCK: ");

  Serial.println(
    LORA_SCK);

  Serial.print(
    "  MISO: ");

  Serial.println(
    LORA_MISO);

  Serial.print(
    "  MOSI: ");

  Serial.println(
    LORA_MOSI);

  Serial.print(
    "  SS: ");

  Serial.println(
    LORA_SS);

  Serial.print(
    "  RST: ");

  Serial.println(
    LORA_RST);

  Serial.print(
    "  DIO0: ");

  Serial.println(
    LORA_DIO0);


  // ----------------------------------------------------------
  // SPI
  //
  // Node 2 has no TFT/touch, so LoRa is the only SPI
  // device on this board - VSPI here is fine, no bus
  // sharing conflict (unlike Node 1).
  // ----------------------------------------------------------

  SPI.begin(
    LORA_SCK,
    LORA_MISO,
    LORA_MOSI,
    LORA_SS);

  LoRa.setPins(
    LORA_SS,
    LORA_RST,
    LORA_DIO0);


  // ----------------------------------------------------------
  // Begin radio
  // ----------------------------------------------------------

  if (!LoRa.begin(LORA_BAND)) {

    Serial.println();
    Serial.println(
      "!!!!!!!!!!!!!!!!!!!!!!!!");

    Serial.println(
      "LoRa initialization FAILED!");

    Serial.println(
      "!!!!!!!!!!!!!!!!!!!!!!!!");

    return false;
  }


  // ----------------------------------------------------------
  // Explicit LoRa configuration
  // ----------------------------------------------------------

  LoRa.setSpreadingFactor(
    LORA_SPREADING_FACTOR);

  LoRa.setSignalBandwidth(
    LORA_BANDWIDTH);

  LoRa.setCodingRate4(
    LORA_CODING_RATE);


  // ----------------------------------------------------------
  // Print configuration
  // ----------------------------------------------------------

  Serial.println();
  Serial.println(
    "LoRa initialization SUCCESS!");

  Serial.println(
    "LoRa configuration:");

  Serial.println(
    "  Frequency: 433 MHz");

  Serial.println(
    "  Spreading Factor: SF7");

  Serial.println(
    "  Bandwidth: 125 kHz");

  Serial.println(
    "  Coding Rate: 4/5");


  // ----------------------------------------------------------
  // Receive mode
  // ----------------------------------------------------------

  LoRa.receive();

  Serial.println();
  Serial.println(
    "LoRa RX MODE ACTIVE");

  return true;
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(2000);


  // ----------------------------------------------------------
  // LED
  // ----------------------------------------------------------

  pinMode(
    LED_PIN,
    OUTPUT);

  digitalWrite(
    LED_PIN,
    LOW);


  // ----------------------------------------------------------
  // Banner
  // ----------------------------------------------------------

  Serial.println();
  Serial.println(
    "========================================");

  Serial.println(
    "BANHA NODE 2");

  Serial.println(
    "LoRa + Supabase + WiFi");

  Serial.println(
    "v12 NON-BLOCKING SUPABASE QUEUE");

  Serial.println(
    "========================================");


  // ----------------------------------------------------------
  // WiFi / Web
  // ----------------------------------------------------------

  startAccessPoint();

  loadSavedWiFi();

  startWebServer();

  startWiFiConnection();


  // ----------------------------------------------------------
  // LoRa
  // ----------------------------------------------------------

  loraReady =
    initializeLoRa();


  // ----------------------------------------------------------
  // Start non-blocking Supabase uploader
  // ----------------------------------------------------------

  startDataUploaderTask();


  // ----------------------------------------------------------
  // Final status
  // ----------------------------------------------------------

  Serial.println();
  Serial.println(
    "========================================");

  Serial.println(
    "NODE 2 READY");

  Serial.print(
    "LoRa: ");

  Serial.println(
    loraReady
      ? "READY"
      : "FAILED");

  Serial.print(
    "Setup WiFi: ");

  Serial.println(
    AP_SSID);

  Serial.print(
    "Setup IP: ");

  Serial.println(
    WiFi.softAPIP());

  Serial.println(
    "========================================");
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  // ----------------------------------------------------------
  // Web server
  // ----------------------------------------------------------

  server.handleClient();


  // ----------------------------------------------------------
  // WiFi
  // ----------------------------------------------------------

  maintainWiFiConnection();


  // ----------------------------------------------------------
  // LoRa
  // ----------------------------------------------------------

  if (!loraReady) {

    delay(5);

    return;
  }


  int packetSize =
    LoRa.parsePacket();


  // ----------------------------------------------------------
  // No packet
  // ----------------------------------------------------------

  if (packetSize <= 0) {

    delay(2);

    return;
  }


  // ----------------------------------------------------------
  // Packet received
  // ----------------------------------------------------------

  Serial.println();
  Serial.println(
    "########################################");

  Serial.print(
    "LoRa PACKET RECEIVED");

  Serial.print(
    " | Size: ");

  Serial.println(
    packetSize);


  String receivedMessage = "";

  while (LoRa.available()) {

    receivedMessage +=
      (char)LoRa.read();
  }


  // ----------------------------------------------------------
  // RSSI
  // ----------------------------------------------------------

  int rssi =
    LoRa.packetRssi();


  // ----------------------------------------------------------
  // SNR
  // ----------------------------------------------------------

  float snr =
    LoRa.packetSnr();


  Serial.print(
    "RSSI: ");

  Serial.print(
    rssi);

  Serial.println(
    " dBm");

  Serial.print(
    "SNR: ");

  Serial.print(
    snr,
    2);

  Serial.println(
    " dB");


  // ----------------------------------------------------------
  // Message
  // ----------------------------------------------------------

  Serial.print(
    "DATA: ");

  Serial.println(
    receivedMessage);


  // ----------------------------------------------------------
  // Process
  // ----------------------------------------------------------

  handleLoRaMessage(
    receivedMessage);


  // ----------------------------------------------------------
  // LED indication
  // ----------------------------------------------------------

  digitalWrite(LED_PIN, HIGH);

  // Do not delay here. Keeping the main loop free is important
  // because LoRa reception is handled here.
  unsigned long ledOffAt = millis() + 50;
  while ((long)(millis() - ledOffAt) < 0) {
    server.handleClient();
    delay(1);
  }

  digitalWrite(LED_PIN, LOW);

  Serial.println(
    "########################################");
}