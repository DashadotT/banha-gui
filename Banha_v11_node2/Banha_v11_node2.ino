/*
   ============================================================
   BANHA LoRa Node 2 - v11 WITH SUPABASE RETRY
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
//
// Shared helper for all three Supabase calls. Retries a
// TRANSIENT failure (timeout, connection error, 5xx) up to
// SUPABASE_MAX_ATTEMPTS times with a short backoff. A 409
// (duplicate key) is returned immediately WITHOUT retrying,
// since retrying a genuine duplicate can never succeed and
// isn't an actual problem - the row is already there.
//
// method: "POST" or "PATCH"
// includePreferHeader: adds "Prefer: return=representation"
//   (used by create/stop, not by the DATA upload)
// outHttpCode / outResponse: filled with the LAST attempt's
//   result, whether it succeeded or all attempts failed.
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

    HTTPClient http;

    http.setConnectTimeout(SUPABASE_HTTP_TIMEOUT_MS);
    http.setTimeout(SUPABASE_HTTP_TIMEOUT_MS);

    http.begin(endpoint);

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

    Serial.print("[Supabase attempt ");
    Serial.print(attempt);
    Serial.print("/");
    Serial.print(SUPABASE_MAX_ATTEMPTS);
    Serial.print("] HTTP Code: ");
    Serial.println(httpCode);

    Serial.print("Response: ");
    Serial.println(response);

    outHttpCode = httpCode;
    outResponse = response;

    // Success.
    if (httpCode >= 200 && httpCode < 300) {
      return true;
    }

    // Duplicate key - not transient, don't retry, treat as
    // "already handled" so callers don't log it as a failure.
    if (httpCode == 409) {
      Serial.println("[Supabase] 409 duplicate - treating as already handled, not retrying.");
      return true;
    }

    // Any other failure (including -1 connection errors) is
    // treated as transient and worth retrying.
    if (attempt < SUPABASE_MAX_ATTEMPTS) {
      Serial.println("[Supabase] Transient failure, retrying shortly...");
      delay(400 * attempt);  // 400ms, then 800ms
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

    Serial.println(
      "Cannot create recording: WiFi unavailable.");

    lastSupabaseStatus =
      "WiFi unavailable";

    return false;
  }

  Serial.println();
  Serial.println(
    "SUPABASE: CREATING RECORDING...");

  lastSupabaseStatus =
    "Creating recording";

  String endpoint =
    String(SUPABASE_URL) + "/rest/v1/recordings";

  String payload = "{";

  payload +=
    "\"device_id\":\"";

  payload +=
    DEVICE_ID;

  payload +=
    "\",\"status\":\"recording\"";

  payload += "}";

  Serial.println(
    "Supabase POST:");

  Serial.println(
    endpoint);

  Serial.println(
    payload);

  int httpCode;
  String response;

  bool ok = supabaseRequest(
    "POST",
    endpoint,
    payload,
    true,
    httpCode,
    response);

  if (!ok) {

    lastSupabaseStatus =
      "Create failed: HTTP " + String(httpCode);

    return false;
  }

  currentRecordingId =
    extractRecordingId(response);

  if (
    currentRecordingId.length() == 0) {

    lastSupabaseStatus =
      "UUID not found";

    return false;
  }

  lastSupabaseStatus =
    "Recording created";

  Serial.print(
    "Recording UUID: ");

  Serial.println(
    currentRecordingId);

  return true;
}


// ============================================================
// UPLOAD ENVIRONMENTAL DATA
// ============================================================

bool uploadEnvironmentalData() {

  if (!isRecordingActive) {

    Serial.println(
      "Upload rejected: Recording inactive.");

    return false;
  }

  if (
    currentRecordingId.length() == 0) {

    Serial.println(
      "Upload rejected: Recording UUID missing.");

    return false;
  }

  if (!ensureWiFi()) {

    Serial.println(
      "Upload failed: WiFi unavailable.");

    lastSupabaseStatus =
      "Upload failed: WiFi unavailable";

    return false;
  }

  lastSupabaseStatus =
    "Uploading reading";

  String endpoint =
    String(SUPABASE_URL) + "/rest/v1/environmental_readings";

  String payload = "{";

  payload +=
    "\"recording_id\":\"";

  payload +=
    currentRecordingId;

  payload +=
    "\",\"packet_number\":" + String(receivedPacketNumber);

  payload +=
    ",\"average_co2\":" + String(receivedAverageCO2, 2);

  payload +=
    ",\"average_temperature\":" + String(receivedAverageTemp, 2);

  payload +=
    ",\"average_noise\":" + String(receivedAverageNoise, 2);

  payload += "}";

  Serial.println();
  Serial.println(
    "SUPABASE: UPLOADING DATA");

  Serial.println(
    payload);

  int httpCode;
  String response;

  bool ok = supabaseRequest(
    "POST",
    endpoint,
    payload,
    false,
    httpCode,
    response);

  if (ok) {

    lastSupabaseStatus =
      (httpCode == 409)
        ? "Reading already exists (dup)"
        : "Reading uploaded";

    Serial.println(
      "SUPABASE: READING UPLOADED (or already existed)!");

    return true;
  }

  lastSupabaseStatus =
    "Upload failed: HTTP " + String(httpCode);

  Serial.println(
    "SUPABASE: UPLOAD FAILED after retries!");

  return false;
}


// ============================================================
// STOP RECORDING IN DATABASE
// ============================================================

bool stopRecordingInDatabase() {

  if (
    currentRecordingId.length() == 0) {

    Serial.println(
      "No recording UUID available.");

    return false;
  }

  if (!ensureWiFi()) {

    lastSupabaseStatus =
      "Stop failed: WiFi unavailable";

    return false;
  }

  lastSupabaseStatus =
    "Stopping recording";

  unsigned long elapsedMilliseconds =
    millis() - recordingStartMillis;

  unsigned long durationSeconds =
    elapsedMilliseconds / 1000;

  Serial.print(
    "Duration seconds: ");

  Serial.println(
    durationSeconds);

  String endpoint =
    String(SUPABASE_URL) + "/rest/v1/recordings?id=eq." + currentRecordingId;

  String payload = "{";

  payload +=
    "\"duration_seconds\":" + String(durationSeconds);

  payload +=
    ",\"status\":\"pending_assessment\"";

  payload += "}";

  int httpCode;
  String response;

  bool ok = supabaseRequest(
    "PATCH",
    endpoint,
    payload,
    true,
    httpCode,
    response);

  if (ok) {

    lastSupabaseStatus =
      "Recording stopped";

    Serial.println(
      "SUPABASE: RECORDING STOPPED!");

    return true;
  }

  lastSupabaseStatus =
    "Stop failed: HTTP " + String(httpCode);

  Serial.println(
    "SUPABASE: STOP FAILED after retries!");

  return false;
}


// ============================================================
// HANDLE START
// ============================================================

void handleStartCommand(
  bool isDuplicate) {

  Serial.println();
  Serial.println(
    "***** START COMMAND *****");

  if (isDuplicate) {

    Serial.println(
      "[DUP] START already processed.");

    return;
  }

  if (isRecordingActive) {

    Serial.println(
      "START ignored: recording already active.");

    return;
  }

  bool created =
    createRecordingInDatabase();

  if (!created) {

    Serial.println(
      "Database recording creation failed.");

    return;
  }

  isRecordingActive = true;

  recordingStartMillis =
    millis();

  lastUploadedPacketNumber = 0;

  Serial.println();
  Serial.println(
    "================================");

  Serial.println(
    "RECORDING SESSION STARTED");

  Serial.print(
    "Recording UUID: ");

  Serial.println(
    currentRecordingId);

  Serial.println(
    "================================");
}


// ============================================================
// HANDLE STOP
// ============================================================

void handleStopCommand(
  bool isDuplicate) {

  Serial.println();
  Serial.println(
    "***** STOP COMMAND *****");

  if (isDuplicate) {

    Serial.println(
      "[DUP] STOP already processed.");

    return;
  }

  if (!isRecordingActive) {

    Serial.println(
      "No active recording.");

    return;
  }

  bool stopped =
    stopRecordingInDatabase();

  if (!stopped) {

    Serial.println(
      "Database stop failed.");

    return;
  }

  isRecordingActive = false;

  Serial.println(
    "RECORDING SESSION STOPPED");

  currentRecordingId = "";

  lastUploadedPacketNumber = 0;
}


// ============================================================
// PARSE DATA
// ============================================================

bool parseDataPacket(
  const String& message) {

  String packetValue =
    getPacketValue(
      message,
      "PACKET:");

  String averageCO2Value =
    getPacketValue(
      message,
      "AVG_CO2:");

  String averageTempValue =
    getPacketValue(
      message,
      "AVG_TEMP:");

  String averageNoiseValue =
    getPacketValue(
      message,
      "AVG_NOISE:");

  if (
    packetValue.length() == 0 || averageCO2Value.length() == 0 || averageTempValue.length() == 0 || averageNoiseValue.length() == 0) {

    return false;
  }

  receivedPacketNumber =
    packetValue.toInt();

  receivedAverageCO2 =
    averageCO2Value.toFloat();

  receivedAverageTemp =
    averageTempValue.toFloat();

  receivedAverageNoise =
    averageNoiseValue.toFloat();

  return true;
}


// ============================================================
// HANDLE DATA
// ============================================================

void handleDataCommand(
  const String& message,
  bool isDuplicateSeqPacket) {

  if (!isRecordingActive) {

    Serial.println(
      "DATA ignored: No active recording.");

    return;
  }

  if (!parseDataPacket(message)) {

    Serial.println(
      "Invalid DATA packet.");

    return;
  }

  Serial.println();
  Serial.println(
    "***** DATA COMMAND *****");

  Serial.print(
    "PACKET: ");

  Serial.println(
    receivedPacketNumber);

  Serial.print(
    "CO2: ");

  Serial.println(
    receivedAverageCO2);

  Serial.print(
    "Temperature: ");

  Serial.println(
    receivedAverageTemp);

  Serial.print(
    "Noise: ");

  Serial.println(
    receivedAverageNoise);


  // ----------------------------------------------------------
  // Duplicate SEQ
  // ----------------------------------------------------------

  if (isDuplicateSeqPacket) {

    Serial.println(
      "[DUP] DATA SEQ already processed.");

    Serial.println(
      "Upload skipped.");

    return;
  }


  // ----------------------------------------------------------
  // Duplicate PACKET
  // ----------------------------------------------------------

  if (
    receivedPacketNumber <= lastUploadedPacketNumber) {

    Serial.print(
      "[DUP] PACKET:");

    Serial.print(
      receivedPacketNumber);

    Serial.println(
      " already uploaded.");

    return;
  }


  // ----------------------------------------------------------
  // Missing packet
  // ----------------------------------------------------------

  if (
    lastUploadedPacketNumber > 0 && receivedPacketNumber > lastUploadedPacketNumber + 1) {

    Serial.print(
      "[MISSING] Expected packet ");

    Serial.print(
      lastUploadedPacketNumber + 1);

    Serial.print(
      " but received ");

    Serial.println(
      receivedPacketNumber);
  }


  // ----------------------------------------------------------
  // Upload
  // ----------------------------------------------------------

  Serial.println(
    "VALID DATA RECEIVED.");

  Serial.println(
    "Uploading to Supabase...");

  bool uploaded =
    uploadEnvironmentalData();

  if (uploaded) {

    lastUploadedPacketNumber =
      receivedPacketNumber;

    Serial.print(
      "Last uploaded PACKET: ");

    Serial.println(
      lastUploadedPacketNumber);

  } else {

    Serial.println(
      "DATA upload failed after retries. This reading is lost.");
  }
}


// ============================================================
// HANDLE LoRa MESSAGE
// ============================================================

void handleLoRaMessage(
  const String& message) {

  Serial.println();
  Serial.println(
    "================================");

  Serial.println(
    "PROCESSING LoRa MESSAGE");

  Serial.print(
    "Message: ");

  Serial.println(
    message);


  // ----------------------------------------------------------
  // Validate Node 1
  // ----------------------------------------------------------

  if (
    !message.startsWith("NODE:1,")) {

    Serial.println(
      "Rejected: not from Node 1.");

    Serial.println(
      "================================");

    return;
  }


  // ----------------------------------------------------------
  // Get TYPE
  // ----------------------------------------------------------

  String packetType =
    getPacketValue(
      message,
      "TYPE:");

  Serial.print(
    "TYPE: ");

  Serial.println(
    packetType);


  if (
    packetType != "START" && packetType != "STOP" && packetType != "DATA") {

    Serial.println(
      "Unknown TYPE.");

    Serial.println(
      "No ACK sent.");

    Serial.println(
      "================================");

    return;
  }


  // ----------------------------------------------------------
  // Get SEQ
  // ----------------------------------------------------------

  String seqStr =
    getPacketValue(
      message,
      "SEQ:");


  // ----------------------------------------------------------
  // Old packet without SEQ
  // ----------------------------------------------------------

  if (seqStr.length() == 0) {

    Serial.println(
      "[WARN] No SEQ field.");

    Serial.println(
      "Processing without ACK.");

    if (packetType == "START") {

      handleStartCommand(false);

    } else if (packetType == "STOP") {

      handleStopCommand(false);

    } else {

      handleDataCommand(
        message,
        false);
    }

    LoRa.receive();

    return;
  }


  // ----------------------------------------------------------
  // Convert SEQ
  // ----------------------------------------------------------

  uint32_t seq =
    (uint32_t)
      strtoul(
        seqStr.c_str(),
        NULL,
        10);


  Serial.print(
    "SEQ: ");

  Serial.println(
    seq);


  // ----------------------------------------------------------
  // Duplicate check
  // ----------------------------------------------------------

  bool duplicate =
    isDuplicateSeq(seq);

  if (duplicate) {

    Serial.println(
      "[DUP] SEQ already seen.");

  } else {

    Serial.println(
      "[NEW] New SEQ.");
  }


  // ----------------------------------------------------------
  // ACK FIRST
  //
  // NOTE: this confirms LoRa DELIVERY only. The Supabase
  // write happens after this and now includes its own
  // retry logic (see supabaseRequest()), but if all
  // retries there still fail, Node 1 will not know, since
  // it already received this ACK. See file header notes.
  // ----------------------------------------------------------

  bool ackSent =
    sendAck(
      seq,
      packetType);

  if (!ackSent) {

    Serial.println(
      "WARNING: ACK failed.");
  }


  // ----------------------------------------------------------
  // Mark sequence
  // ----------------------------------------------------------

  if (!duplicate) {

    markSeqProcessed(seq);
  }


  // ----------------------------------------------------------
  // Process command
  // ----------------------------------------------------------

  if (packetType == "START") {

    handleStartCommand(
      duplicate);

  } else if (packetType == "STOP") {

    handleStopCommand(
      duplicate);

  } else if (packetType == "DATA") {

    handleDataCommand(
      message,
      duplicate);
  }


  // ----------------------------------------------------------
  // ALWAYS RECEIVE
  // ----------------------------------------------------------

  LoRa.receive();

  Serial.println(
    "LoRa RX MODE: READY");

  Serial.println(
    "================================");
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
    "v11 WITH SUPABASE RETRY");

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

  digitalWrite(
    LED_PIN,
    HIGH);

  delay(50);

  digitalWrite(
    LED_PIN,
    LOW);


  Serial.println(
    "########################################");
}
