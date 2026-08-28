/*
   ====================================================
   BANHA LoRa Node 2
   RECEIVER + SUPABASE UPLOADER + WIFI WEB CONFIG
   ====================================================

   FEATURES:

   1. LoRa Receiver
      Receives START, DATA, and STOP from Node 1.

   2. Supabase Uploader
      START -> Creates recording
      DATA  -> Uploads environmental readings
      STOP  -> Stops recording

   3. Persistent WiFi Configuration
      WiFi credentials are stored using Preferences.

   4. Always Available WiFi Setup Portal

      ESP32 runs in WIFI_AP_STA mode.

      AP:
      SSID: BANHA-SETUP
      PASSWORD: 12345678
      URL: http://192.168.4.1

      IMPORTANT:
      The BANHA-SETUP hotspot remains available even when
      ESP32 is trying to connect to another WiFi network.

   5. NON-BLOCKING WIFI CONNECTION

      Wrong WiFi credentials will NOT freeze the web server.

      The ESP32:
      - Keeps BANHA-SETUP active
      - Keeps web dashboard responsive
      - Tries router connection in background
      - Times out after 15 seconds
      - Retries every 15 seconds

   ====================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <SPI.h>
#include <LoRa.h>


// =====================================================
// ACCESS POINT SETTINGS
// =====================================================

const char* AP_SSID =
  "BANHA-SETUP";

const char* AP_PASSWORD =
  "12345678";


// =====================================================
// WEB SERVER
// =====================================================

WebServer server(80);


// =====================================================
// PREFERENCES
// =====================================================

Preferences preferences;


// =====================================================
// SAVED WIFI
// =====================================================

String savedWiFiSSID = "";
String savedWiFiPassword = "";


// =====================================================
// WIFI STATUS
// =====================================================

bool wifiConnected = false;

bool wifiConnectionAttempting = false;


// Time when current WiFi connection attempt started

unsigned long wifiConnectStartMillis = 0;


// Time when last WiFi attempt started/ended

unsigned long lastWiFiReconnectAttempt = 0;


// Maximum time for one WiFi connection attempt

const unsigned long WIFI_CONNECT_TIMEOUT = 15000;


// Wait before trying again

const unsigned long WIFI_RECONNECT_INTERVAL = 15000;


// =====================================================
// SUPABASE SETTINGS
// =====================================================

const char* SUPABASE_URL =
  "https://jeiolvlujtnmkwprppdu.supabase.co";


// Use ANON / PUBLISHABLE KEY
// DO NOT USE SERVICE_ROLE KEY

const char* SUPABASE_KEY =
  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImplaW9sdmx1anRubWt3cHJwcGR1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODc2NjQwNDksImV4cCI6MjEwMzI0MDA0OX0.I6vbxGR_mdU4p7hTxs9b2ASgBnd_5ltavPwlRuDdXr8";


// =====================================================
// BANHA TEST DEVICE UUID
// =====================================================

const char* DEVICE_ID =
  "11111111-1111-1111-1111-111111111111";


// =====================================================
// BUILT-IN LED
// =====================================================

#define LED_PIN 2


// =====================================================
// LORA PINS
// =====================================================

#define LORA_SCK 23
#define LORA_MISO 19
#define LORA_MOSI 17

#define LORA_SS 16
#define LORA_RST 14
#define LORA_DIO0 26


// =====================================================
// LORA FREQUENCY
// =====================================================

#define LORA_BAND 433E6


// =====================================================
// LORA STATUS
// =====================================================

bool loraReady = false;


// =====================================================
// SUPABASE STATUS
// =====================================================

String lastSupabaseStatus =
  "Waiting";


// =====================================================
// RECORDING SESSION
// =====================================================

bool isRecordingActive = false;


// UUID from Supabase recordings.id

String currentRecordingId = "";


// Local start time for duration calculation

unsigned long recordingStartMillis = 0;


// =====================================================
// RECEIVED DATA
// =====================================================

unsigned long receivedPacketNumber = 0;

float receivedAverageCO2 = 0.0f;

float receivedAverageTemp = 0.0f;

float receivedAverageNoise = 0.0f;


// =====================================================
// START ACCESS POINT
// =====================================================

void startAccessPoint() {

  Serial.println();
  Serial.println("Starting BANHA WiFi Setup AP...");


  // AP + Station mode simultaneously

  WiFi.mode(WIFI_AP_STA);


  bool started =
    WiFi.softAP(
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


// =====================================================
// LOAD SAVED WIFI
// =====================================================

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

    Serial.println("No saved WiFi configuration.");

    return;
  }


  Serial.print("Saved SSID: ");
  Serial.println(savedWiFiSSID);
}


// =====================================================
// SAVE WIFI
// =====================================================

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


  savedWiFiSSID =
    ssid;


  savedWiFiPassword =
    password;


  Serial.println("WiFi credentials saved.");
}


// =====================================================
// START WIFI CONNECTION
//
// NON-BLOCKING
// Does NOT wait in a while() loop.
// =====================================================

void startWiFiConnection() {

  if (savedWiFiSSID.length() == 0) {

    Serial.println("No WiFi configured.");

    wifiConnected = false;
    wifiConnectionAttempting = false;

    return;
  }


  // Already connected

  if (WiFi.status() == WL_CONNECTED) {

    wifiConnected = true;
    wifiConnectionAttempting = false;

    return;
  }


  Serial.println();
  Serial.println("Starting WiFi connection...");

  Serial.print("SSID: ");
  Serial.println(savedWiFiSSID);


  // Keep AP active

  WiFi.mode(WIFI_AP_STA);


  // Start connection.
  // This does NOT block the web server.

  WiFi.begin(
    savedWiFiSSID.c_str(),
    savedWiFiPassword.c_str());


  wifiConnectionAttempting = true;


  wifiConnectStartMillis =
    millis();


  lastWiFiReconnectAttempt =
    millis();
}


// =====================================================
// MAINTAIN WIFI CONNECTION
//
// NON-BLOCKING
//
// Call continuously inside loop().
// =====================================================

void maintainWiFiConnection() {

  // ---------------------------------------------------
  // ALREADY CONNECTED
  // ---------------------------------------------------

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


  // ---------------------------------------------------
  // NOT CONNECTED
  // ---------------------------------------------------

  wifiConnected = false;


  // ---------------------------------------------------
  // CONNECTION CURRENTLY IN PROGRESS
  // ---------------------------------------------------

  if (wifiConnectionAttempting) {

    // Still within timeout period

    if (
      millis() - wifiConnectStartMillis < WIFI_CONNECT_TIMEOUT) {

      return;
    }


    // Connection attempt timed out

    Serial.println();
    Serial.println("WiFi connection attempt timed out.");

    Serial.println(
      "BANHA-SETUP remains available.");


    WiFi.disconnect(
      false,
      false);


    wifiConnectionAttempting = false;


    lastWiFiReconnectAttempt =
      millis();


    return;
  }


  // ---------------------------------------------------
  // WAIT BEFORE RETRYING
  // ---------------------------------------------------

  if (savedWiFiSSID.length() == 0) {

    return;
  }


  if (
    millis() - lastWiFiReconnectAttempt < WIFI_RECONNECT_INTERVAL) {

    return;
  }


  Serial.println();
  Serial.println("Retrying configured WiFi...");


  startWiFiConnection();
}


// =====================================================
// ENSURE WIFI FOR SUPABASE
//
// This does NOT block.
// =====================================================

bool ensureWiFi() {

  if (WiFi.status() == WL_CONNECTED) {

    wifiConnected = true;

    return true;
  }


  wifiConnected = false;


  // Start connection if not already trying

  if (!wifiConnectionAttempting) {

    if (
      savedWiFiSSID.length() > 0 && millis() - lastWiFiReconnectAttempt >= WIFI_RECONNECT_INTERVAL) {

      startWiFiConnection();
    }
  }


  return false;
}


// =====================================================
// HTML PAGE
// =====================================================

String getDashboardHTML() {

  String wifiStatus;


  if (WiFi.status() == WL_CONNECTED) {

    wifiStatus = "Connected";

  } else if (wifiConnectionAttempting) {

    wifiStatus = "Connecting...";

  } else {

    wifiStatus = "Disconnected";
  }


  String recordingStatus;


  if (isRecordingActive) {

    recordingStatus = "RECORDING";

  } else {

    recordingStatus = "IDLE";
  }


  String html =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<meta charset='UTF-8'>"

    "<title>BANHA Node 2 Setup</title>"

    "<style>"

    "*{box-sizing:border-box;}"

    "body{"
    "margin:0;"
    "font-family:Arial,sans-serif;"
    "background:#f1f5f9;"
    "color:#1e293b;"
    "}"

    ".container{"
    "max-width:650px;"
    "margin:30px auto;"
    "padding:20px;"
    "}"

    ".card{"
    "background:white;"
    "padding:25px;"
    "border-radius:15px;"
    "margin-bottom:20px;"
    "box-shadow:0 4px 15px rgba(0,0,0,.08);"
    "}"

    "h1{"
    "margin-top:0;"
    "color:#15803d;"
    "}"

    "h2{"
    "font-size:18px;"
    "border-bottom:1px solid #e2e8f0;"
    "padding-bottom:10px;"
    "}"

    ".status{"
    "padding:12px;"
    "background:#f8fafc;"
    "border-radius:8px;"
    "margin:8px 0;"
    "word-break:break-word;"
    "}"

    "label{"
    "display:block;"
    "margin-top:15px;"
    "font-weight:bold;"
    "}"

    "input{"
    "width:100%;"
    "padding:13px;"
    "margin-top:6px;"
    "border:1px solid #cbd5e1;"
    "border-radius:8px;"
    "font-size:16px;"
    "}"

    "button{"
    "width:100%;"
    "padding:14px;"
    "margin-top:20px;"
    "background:#15803d;"
    "color:white;"
    "border:none;"
    "border-radius:8px;"
    "font-size:16px;"
    "font-weight:bold;"
    "cursor:pointer;"
    "}"

    ".small{"
    "font-size:13px;"
    "color:#64748b;"
    "}"

    "</style>"
    "</head>"

    "<body>"

    "<div class='container'>"

    "<div class='card'>"

    "<h1>BANHA Node 2</h1>"

    "<p class='small'>"
    "WiFi Configuration Dashboard"
    "</p>"


    "<div class='status'>"
    "<b>Setup WiFi:</b> "
    + String(AP_SSID)
    + "</div>"


      "<div class='status'>"
      "<b>Setup IP:</b> "
    + WiFi.softAPIP().toString()
    + "</div>"


      "<div class='status'>"
      "<b>Router Status:</b> "
    + wifiStatus
    + "</div>"


      "<div class='status'>"
      "<b>Configured WiFi:</b> "
    + (savedWiFiSSID.length() > 0
         ? savedWiFiSSID
         : String("None"))
    + "</div>"


      "<div class='status'>"
      "<b>Connected SSID:</b> "
    + (WiFi.status() == WL_CONNECTED
         ? WiFi.SSID()
         : String("None"))
    + "</div>"


      "<div class='status'>"
      "<b>Router IP:</b> "
    + (WiFi.status() == WL_CONNECTED
         ? WiFi.localIP().toString()
         : String("None"))
    + "</div>"


      "<div class='status'>"
      "<b>LoRa:</b> "
    + (loraReady
         ? String("Ready")
         : String("Not Ready"))
    + "</div>"


      "<div class='status'>"
      "<b>Recording:</b> "
    + recordingStatus
    + "</div>"


      "<div class='status'>"
      "<b>Supabase:</b> "
    + lastSupabaseStatus
    + "</div>"


      "</div>"


      // ================================================
      // WIFI FORM
      // ================================================

      "<div class='card'>"

      "<h2>Change WiFi Configuration</h2>"

      "<form action='/save' method='POST'>"


      "<label>WiFi Name (SSID)</label>"

      "<input "
      "type='text' "
      "name='ssid' "
      "autocomplete='off' "
      "value='"
    + savedWiFiSSID
    + "' "
      "required>"


      "<label>WiFi Password</label>"

      "<input "
      "type='password' "
      "name='password' "
      "autocomplete='new-password' "
      "placeholder='Enter WiFi password'>"


      "<button type='submit'>"
      "Save and Connect"
      "</button>"


      "</form>"

      "</div>"


      // ================================================
      // INSTRUCTIONS
      // ================================================

      "<div class='card'>"

      "<h2>How to Access</h2>"

      "<p>"
      "1. Connect your phone to "
      "<b>BANHA-SETUP</b>."
      "</p>"

      "<p>"
      "2. Password: "
      "<b>12345678</b>"
      "</p>"

      "<p>"
      "3. Open "
      "<b>192.168.4.1</b>"
      "</p>"

      "<p class='small'>"
      "The BANHA-SETUP WiFi stays active even if the "
      "configured router WiFi has wrong credentials."
      "</p>"

      "</div>"

      "</div>"

      "</body>"
      "</html>";


  return html;
}


// =====================================================
// WEB: HOME
// =====================================================

void handleRoot() {

  server.send(
    200,
    "text/html",
    getDashboardHTML());
}


// =====================================================
// WEB: SAVE WIFI
// =====================================================

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


  // ---------------------------------------------------
  // SAVE NEW CREDENTIALS
  // ---------------------------------------------------

  saveWiFi(
    newSSID,
    newPassword);


  Serial.println();

  Serial.println(
    "New WiFi configuration received from dashboard.");

  Serial.print("SSID: ");
  Serial.println(newSSID);


  // ---------------------------------------------------
  // SEND RESPONSE FIRST
  //
  // This is important so the browser receives the
  // response before WiFi operations start.
  // ---------------------------------------------------

  String response =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<meta charset='UTF-8'>"
    "</head>"

    "<body style='font-family:Arial;text-align:center;padding:40px;'>"

    "<h2>WiFi Saved!</h2>"

    "<p>Node 2 will now connect to:</p>"

    "<h3>"
    + newSSID
    + "</h3>"

      "<p>BANHA-SETUP remains available.</p>"

      "<p>"
      "<a href='/'>Return to Dashboard</a>"
      "</p>"

      "</body>"
      "</html>";


  server.send(
    200,
    "text/html",
    response);


  // ---------------------------------------------------
  // CANCEL OLD CONNECTION ATTEMPT
  // ---------------------------------------------------

  wifiConnectionAttempting = false;


  // Disconnect STA only.
  // AP remains active.

  WiFi.disconnect(
    false,
    false);


  lastWiFiReconnectAttempt = 0;


  // ---------------------------------------------------
  // START NEW CONNECTION IN BACKGROUND
  //
  // NO delay()
  // NO while() loop
  // ---------------------------------------------------

  startWiFiConnection();
}


// =====================================================
// WEB: STATUS API
// =====================================================

void handleStatus() {

  String json =
    "{";


  json +=
    "\"wifi_connected\":"
    + String(
      WiFi.status() == WL_CONNECTED
        ? "true"
        : "false");


  json +=
    ",\"wifi_connecting\":"
    + String(
      wifiConnectionAttempting
        ? "true"
        : "false");


  json +=
    ",\"configured_ssid\":\""
    + savedWiFiSSID
    + "\"";


  json +=
    ",\"ssid\":\""
    + (WiFi.status() == WL_CONNECTED
         ? WiFi.SSID()
         : String(""))
    + "\"";


  json +=
    ",\"ip\":\""
    + (WiFi.status() == WL_CONNECTED
         ? WiFi.localIP().toString()
         : String(""))
    + "\"";


  json +=
    ",\"ap_ssid\":\""
    + String(AP_SSID)
    + "\"";


  json +=
    ",\"ap_ip\":\""
    + WiFi.softAPIP().toString()
    + "\"";


  json +=
    ",\"lora_ready\":"
    + String(
      loraReady
        ? "true"
        : "false");


  json +=
    ",\"recording_active\":"
    + String(
      isRecordingActive
        ? "true"
        : "false");


  json +=
    "}";


  server.send(
    200,
    "application/json",
    json);
}


// =====================================================
// START WEB SERVER
// =====================================================

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


// =====================================================
// GET VALUE FROM LORA PACKET
// =====================================================

String getPacketValue(
  String message,
  String key) {

  int startIndex =
    message.indexOf(key);


  if (startIndex == -1) {

    return "";
  }


  startIndex +=
    key.length();


  int endIndex =
    message.indexOf(
      ",",
      startIndex);


  if (endIndex == -1) {

    endIndex =
      message.length();
  }


  return message.substring(
    startIndex,
    endIndex);
}


// =====================================================
// EXTRACT RECORDING UUID
// =====================================================

String extractRecordingId(
  String response) {

  int idKey =
    response.indexOf(
      "\"id\":\"");


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


// =====================================================
// CREATE RECORDING IN SUPABASE
// =====================================================

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
    String(SUPABASE_URL)
    + "/rest/v1/recordings";


  HTTPClient http;


  http.begin(endpoint);


  http.addHeader(
    "Content-Type",
    "application/json");


  http.addHeader(
    "apikey",
    SUPABASE_KEY);


  http.addHeader(
    "Authorization",
    String("Bearer ")
      + SUPABASE_KEY);


  http.addHeader(
    "Prefer",
    "return=representation");


  String payload =
    "{";


  payload +=
    "\"device_id\":\"";

  payload +=
    DEVICE_ID;

  payload +=
    "\",";


  payload +=
    "\"status\":\"recording\"";


  payload +=
    "}";


  Serial.println("POST:");
  Serial.println(endpoint);

  Serial.println("Payload:");
  Serial.println(payload);


  int httpCode =
    http.POST(payload);


  String response =
    http.getString();


  Serial.print("HTTP Code: ");
  Serial.println(httpCode);


  Serial.print("Response: ");
  Serial.println(response);


  http.end();


  if (
    httpCode < 200 || httpCode >= 300) {

    lastSupabaseStatus =
      "Create failed: HTTP "
      + String(httpCode);


    return false;
  }


  currentRecordingId =
    extractRecordingId(
      response);


  if (
    currentRecordingId.length() == 0) {

    lastSupabaseStatus =
      "UUID not found";


    return false;
  }


  lastSupabaseStatus =
    "Recording created";


  Serial.print("Recording UUID: ");
  Serial.println(currentRecordingId);


  return true;
}


// =====================================================
// UPLOAD ENVIRONMENTAL DATA
// =====================================================

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
    String(SUPABASE_URL)
    + "/rest/v1/environmental_readings";


  HTTPClient http;


  http.begin(endpoint);


  http.addHeader(
    "Content-Type",
    "application/json");


  http.addHeader(
    "apikey",
    SUPABASE_KEY);


  http.addHeader(
    "Authorization",
    String("Bearer ")
      + SUPABASE_KEY);


  String payload =
    "{";


  payload +=
    "\"recording_id\":\"";

  payload +=
    currentRecordingId;

  payload +=
    "\",";


  payload +=
    "\"packet_number\":"
    + String(receivedPacketNumber)
    + ",";


  payload +=
    "\"average_co2\":"
    + String(receivedAverageCO2, 2)
    + ",";


  payload +=
    "\"average_temperature\":"
    + String(receivedAverageTemp, 2)
    + ",";


  payload +=
    "\"average_noise\":"
    + String(receivedAverageNoise, 2);


  payload +=
    "}";


  Serial.println();
  Serial.println(
    "SUPABASE: UPLOADING READING...");

  Serial.println(payload);


  int httpCode =
    http.POST(payload);


  String response =
    http.getString();


  Serial.print("HTTP Code: ");
  Serial.println(httpCode);


  Serial.print("Response: ");
  Serial.println(response);


  http.end();


  if (
    httpCode >= 200 && httpCode < 300) {

    lastSupabaseStatus =
      "Reading uploaded";


    Serial.println(
      "SUPABASE: READING UPLOADED!");


    return true;
  }


  lastSupabaseStatus =
    "Upload failed: HTTP "
    + String(httpCode);


  Serial.println(
    "SUPABASE: READING UPLOAD FAILED!");


  return false;
}


// =====================================================
// STOP RECORDING IN SUPABASE
// =====================================================

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


  int durationSeconds =
    elapsedMilliseconds / 1000;


  Serial.print(
    "Duration seconds: ");

  Serial.println(
    durationSeconds);


  String endpoint =
    String(SUPABASE_URL)
    + "/rest/v1/recordings?id=eq."
    + currentRecordingId;


  HTTPClient http;


  http.begin(endpoint);


  http.addHeader(
    "Content-Type",
    "application/json");


  http.addHeader(
    "apikey",
    SUPABASE_KEY);


  http.addHeader(
    "Authorization",
    String("Bearer ")
      + SUPABASE_KEY);


  http.addHeader(
    "Prefer",
    "return=representation");


  String payload =
    "{";


  payload +=
    "\"duration_seconds\":"
    + String(durationSeconds)
    + ",";


  payload +=
    "\"status\":\"pending_assessment\"";


  payload +=
    "}";


  Serial.println("PATCH:");
  Serial.println(endpoint);

  Serial.println("Payload:");
  Serial.println(payload);


  int httpCode =
    http.sendRequest(
      "PATCH",
      payload);


  String response =
    http.getString();


  Serial.print("HTTP Code: ");
  Serial.println(httpCode);


  Serial.print("Response: ");
  Serial.println(response);


  http.end();


  if (
    httpCode >= 200 && httpCode < 300) {

    lastSupabaseStatus =
      "Recording stopped";


    Serial.println(
      "SUPABASE: RECORDING STOPPED!");


    return true;
  }


  lastSupabaseStatus =
    "Stop failed: HTTP "
    + String(httpCode);


  Serial.println(
    "SUPABASE: FAILED TO STOP RECORDING!");


  return false;
}


// =====================================================
// HANDLE START COMMAND
// =====================================================

void handleStartCommand() {

  Serial.println();
  Serial.println(
    "***** START COMMAND RECEIVED *****");


  if (isRecordingActive) {

    Serial.println(
      "Duplicate START ignored.");

    return;
  }


  bool created =
    createRecordingInDatabase();


  if (!created) {

    Serial.println(
      "Database recording creation failed.");

    return;
  }


  isRecordingActive =
    true;


  recordingStartMillis =
    millis();


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


// =====================================================
// HANDLE STOP COMMAND
// =====================================================

void handleStopCommand() {

  Serial.println();
  Serial.println(
    "***** STOP COMMAND RECEIVED *****");


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


  isRecordingActive =
    false;


  Serial.println(
    "RECORDING SESSION STOPPED");


  currentRecordingId =
    "";
}


// =====================================================
// PARSE DATA PACKET
// =====================================================

bool parseDataPacket(
  String message) {

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


// =====================================================
// HANDLE LORA MESSAGE
// =====================================================

void handleLoRaMessage(
  String message) {

  if (
    !message.startsWith(
      "NODE:1,")) {

    Serial.println(
      "Rejected: Not from Node 1.");

    return;
  }


  String packetType =
    getPacketValue(
      message,
      "TYPE:");


  Serial.print(
    "Packet TYPE: ");

  Serial.println(
    packetType);


  // START

  if (packetType == "START") {

    handleStartCommand();

    return;
  }


  // STOP

  if (packetType == "STOP") {

    handleStopCommand();

    return;
  }


  // DATA

  if (packetType == "DATA") {

    if (!isRecordingActive) {

      Serial.println(
        "DATA ignored: No active recording.");

      return;
    }


    bool valid =
      parseDataPacket(
        message);


    if (!valid) {

      Serial.println(
        "Invalid DATA packet.");

      return;
    }


    Serial.println(
      "VALID DATA RECEIVED");


    Serial.print("Packet: ");
    Serial.println(receivedPacketNumber);


    Serial.print("CO2: ");
    Serial.println(receivedAverageCO2);


    Serial.print("Temperature: ");
    Serial.println(receivedAverageTemp);


    Serial.print("Noise: ");
    Serial.println(receivedAverageNoise);


    uploadEnvironmentalData();

    return;
  }


  Serial.println(
    "Unknown packet TYPE.");
}


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);


  delay(2000);


  pinMode(
    LED_PIN,
    OUTPUT);


  digitalWrite(
    LED_PIN,
    LOW);


  Serial.println();
  Serial.println(
    "================================");

  Serial.println(
    "BANHA NODE 2");

  Serial.println(
    "LORA + SUPABASE + WIFI SETUP");

  Serial.println(
    "================================");


  // ---------------------------------------------------
  // ALWAYS START BANHA-SETUP
  // ---------------------------------------------------

  startAccessPoint();


  // ---------------------------------------------------
  // LOAD SAVED WIFI
  // ---------------------------------------------------

  loadSavedWiFi();


  // ---------------------------------------------------
  // START WEB SERVER FIRST
  //
  // This ensures the dashboard is available immediately.
  // ---------------------------------------------------

  startWebServer();


  // ---------------------------------------------------
  // CONNECT TO SAVED WIFI IN BACKGROUND
  //
  // Non-blocking.
  // ---------------------------------------------------

  startWiFiConnection();


  // ---------------------------------------------------
  // LORA SPI
  // ---------------------------------------------------

  SPI.begin(
    LORA_SCK,
    LORA_MISO,
    LORA_MOSI,
    LORA_SS);


  LoRa.setPins(
    LORA_SS,
    LORA_RST,
    LORA_DIO0);


  Serial.println(
    "Starting LoRa...");


  if (!LoRa.begin(LORA_BAND)) {

    Serial.println(
      "LoRa FAILED!");


    loraReady = false;

  } else {

    loraReady = true;


    Serial.println(
      "LoRa Ready!");
  }


  Serial.println();
  Serial.println(
    "================================");

  Serial.println(
    "NODE 2 READY");


  Serial.print(
    "Always available setup WiFi: ");

  Serial.println(
    AP_SSID);


  Serial.print(
    "Setup address: http://");

  Serial.println(
    WiFi.softAPIP());


  Serial.println(
    "================================");
}


// =====================================================
// LOOP
// =====================================================

void loop() {

  // ---------------------------------------------------
  // HIGHEST PRIORITY:
  // KEEP WEB DASHBOARD RESPONSIVE
  // ---------------------------------------------------

  server.handleClient();


  // ---------------------------------------------------
  // MAINTAIN WIFI IN BACKGROUND
  //
  // This does not contain blocking while() loops.
  // ---------------------------------------------------

  maintainWiFiConnection();


  // ---------------------------------------------------
  // STOP LORA PROCESSING IF LORA FAILED
  //
  // The web dashboard still works.
  // ---------------------------------------------------

  if (!loraReady) {

    delay(5);

    return;
  }


  // ---------------------------------------------------
  // CHECK LORA
  // ---------------------------------------------------

  int packetSize =
    LoRa.parsePacket();


  if (packetSize <= 0) {

    delay(5);

    return;
  }


  String receivedMessage =
    "";


  while (LoRa.available()) {

    char c =
      (char)LoRa.read();


    receivedMessage +=
      c;
  }


  Serial.println();

  Serial.println(
    "================================");


  Serial.print(
    "LoRa: ");

  Serial.println(
    receivedMessage);


  handleLoRaMessage(
    receivedMessage);


  Serial.print(
    "RSSI: ");

  Serial.println(
    LoRa.packetRssi());


  Serial.println(
    "================================");


  // ---------------------------------------------------
  // BLINK LED WHEN PACKET RECEIVED
  // ---------------------------------------------------

  digitalWrite(
    LED_PIN,
    HIGH);


  delay(100);


  digitalWrite(
    LED_PIN,
    LOW);
}