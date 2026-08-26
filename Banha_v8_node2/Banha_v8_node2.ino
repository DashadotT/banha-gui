/*
   ====================================================
   BANHA LoRa Node 2
   RECEIVER + SUPABASE UPLOADER
   ====================================================

   Receives:

   START:
   NODE:1,TYPE:START

   DATA:
   NODE:1,TYPE:DATA,PACKET:1,AVG_CO2:842.5,AVG_TEMP:29.8,AVG_NOISE:51.8

   STOP:
   NODE:1,TYPE:STOP


   DATABASE FLOW:

   START
   ↓
   INSERT INTO recordings
   ↓
   Receive recording UUID
   ↓
   Save UUID in currentRecordingId
   ↓
   DATA
   ↓
   INSERT INTO environmental_readings
   using currentRecordingId
   ↓
   STOP
   ↓
   UPDATE recordings
   ended_at = now()
   duration_seconds = calculated duration
   status = completed
*/


#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <SPI.h>
#include <LoRa.h>


// =====================================================
// WIFI SETTINGS
// =====================================================

const char* WIFI_SSID =
  "Octat 2.4G";

const char* WIFI_PASSWORD =
  "Sunshine3030z";


// =====================================================
// SUPABASE SETTINGS
// =====================================================

// Example:
// https://abcdefghijk.supabase.co

const char* SUPABASE_URL =
  "https://txklhdloetfxbezafqzu.supabase.co";


// Use your ANON / PUBLISHABLE KEY
// DO NOT USE SERVICE_ROLE KEY

const char* SUPABASE_KEY =
  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InR4a2xoZGxvZXRmeGJlemFmcXp1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODc3NDk3MzcsImV4cCI6MjEwMzMyNTczN30.bGXQM_n9fz0TwRAs8C6wpFzPi2uE5A-qDhO71QgXxKA";


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
// WIFI CONNECT
// =====================================================

void connectWiFi() {

  Serial.println();

  Serial.println(
    "Connecting to WiFi..."
  );


  WiFi.mode(
    WIFI_STA
  );


  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );


  unsigned long startAttempt =
    millis();


  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - startAttempt < 20000
  ) {

    Serial.print(
      "."
    );

    delay(500);
  }


  Serial.println();


  if (
    WiFi.status() == WL_CONNECTED
  ) {

    Serial.println(
      "WiFi connected!"
    );


    Serial.print(
      "IP Address: "
    );

    Serial.println(
      WiFi.localIP()
    );

  } else {

    Serial.println(
      "WiFi connection FAILED!"
    );
  }
}


// =====================================================
// ENSURE WIFI CONNECTION
// =====================================================

bool ensureWiFi() {

  if (
    WiFi.status() == WL_CONNECTED
  ) {

    return true;
  }


  Serial.println(
    "WiFi disconnected. Reconnecting..."
  );


  connectWiFi();


  return
    WiFi.status() == WL_CONNECTED;
}


// =====================================================
// GET VALUE FROM LORA PACKET
// =====================================================

String getPacketValue(
  String message,
  String key
) {

  int startIndex =
    message.indexOf(
      key
    );


  if (
    startIndex == -1
  ) {

    return "";
  }


  startIndex +=
    key.length();


  int endIndex =
    message.indexOf(
      ",",
      startIndex
    );


  if (
    endIndex == -1
  ) {

    endIndex =
      message.length();
  }


  return
    message.substring(
      startIndex,
      endIndex
    );
}


// =====================================================
// EXTRACT RECORDING UUID
//
// Supabase POST response example:
//
// [
//   {
//     "id":"xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
//   }
// ]
// =====================================================

String extractRecordingId(
  String response
) {

  int idKey =
    response.indexOf(
      "\"id\":\""
    );


  if (
    idKey == -1
  ) {

    return "";
  }


  idKey +=
    6;


  int endQuote =
    response.indexOf(
      "\"",
      idKey
    );


  if (
    endQuote == -1
  ) {

    return "";
  }


  return
    response.substring(
      idKey,
      endQuote
    );
}


// =====================================================
// CREATE RECORDING IN SUPABASE
// =====================================================

bool createRecordingInDatabase() {

  if (
    !ensureWiFi()
  ) {

    Serial.println(
      "Cannot create recording: WiFi unavailable."
    );

    return false;
  }


  Serial.println();

  Serial.println(
    "SUPABASE: CREATING RECORDING..."
  );


  String endpoint =
    String(SUPABASE_URL) +
    "/rest/v1/recordings";


  HTTPClient http;


  http.begin(
    endpoint
  );


  http.addHeader(
    "Content-Type",
    "application/json"
  );


  http.addHeader(
    "apikey",
    SUPABASE_KEY
  );


  http.addHeader(
    "Authorization",
    String("Bearer ") +
    SUPABASE_KEY
  );


  // IMPORTANT:
  // Return the inserted row so ESP32 can get UUID

  http.addHeader(
    "Prefer",
    "return=representation"
  );


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


  Serial.println(
    "POST:"
  );

  Serial.println(
    endpoint
  );


  Serial.println(
    "Payload:"
  );

  Serial.println(
    payload
  );


  int httpCode =
    http.POST(
      payload
    );


  String response =
    http.getString();


  Serial.print(
    "HTTP Code: "
  );

  Serial.println(
    httpCode
  );


  Serial.print(
    "Response: "
  );

  Serial.println(
    response
  );


  http.end();


  if (
    httpCode < 200 ||
    httpCode >= 300
  ) {

    Serial.println(
      "SUPABASE: Failed to create recording."
    );

    return false;
  }


  currentRecordingId =
    extractRecordingId(
      response
    );


  if (
    currentRecordingId.length() == 0
  ) {

    Serial.println(
      "SUPABASE: Recording UUID not found!"
    );

    return false;
  }


  Serial.println(
    "SUPABASE: RECORDING CREATED!"
  );


  Serial.print(
    "Recording UUID: "
  );

  Serial.println(
    currentRecordingId
  );


  return true;
}


// =====================================================
// UPLOAD ENVIRONMENTAL DATA
// =====================================================

bool uploadEnvironmentalData() {

  if (
    !isRecordingActive
  ) {

    Serial.println(
      "Upload rejected: Recording inactive."
    );

    return false;
  }


  if (
    currentRecordingId.length() == 0
  ) {

    Serial.println(
      "Upload rejected: Recording UUID missing."
    );

    return false;
  }


  if (
    !ensureWiFi()
  ) {

    Serial.println(
      "Upload failed: WiFi unavailable."
    );

    return false;
  }


  String endpoint =
    String(SUPABASE_URL) +
    "/rest/v1/environmental_readings";


  HTTPClient http;


  http.begin(
    endpoint
  );


  http.addHeader(
    "Content-Type",
    "application/json"
  );


  http.addHeader(
    "apikey",
    SUPABASE_KEY
  );


  http.addHeader(
    "Authorization",
    String("Bearer ") +
    SUPABASE_KEY
  );


  String payload =
    "{";


  payload +=
    "\"recording_id\":\"";

  payload +=
    currentRecordingId;

  payload +=
    "\",";


  payload +=
    "\"packet_number\":";

  payload +=
    String(
      receivedPacketNumber
    );

  payload +=
    ",";


  payload +=
    "\"average_co2\":";

  payload +=
    String(
      receivedAverageCO2,
      2
    );

  payload +=
    ",";


  payload +=
    "\"average_temperature\":";

  payload +=
    String(
      receivedAverageTemp,
      2
    );

  payload +=
    ",";


  payload +=
    "\"average_noise\":";

  payload +=
    String(
      receivedAverageNoise,
      2
    );


  payload +=
    "}";


  Serial.println();

  Serial.println(
    "SUPABASE: UPLOADING READING..."
  );


  Serial.println(
    payload
  );


  int httpCode =
    http.POST(
      payload
    );


  String response =
    http.getString();


  Serial.print(
    "HTTP Code: "
  );

  Serial.println(
    httpCode
  );


  Serial.print(
    "Response: "
  );

  Serial.println(
    response
  );


  http.end();


  if (
    httpCode >= 200 &&
    httpCode < 300
  ) {

    Serial.println(
      "SUPABASE: READING UPLOADED!"
    );

    return true;
  }


  Serial.println(
    "SUPABASE: READING UPLOAD FAILED!"
  );


  return false;
}


// =====================================================
// STOP RECORDING IN SUPABASE
// =====================================================

bool stopRecordingInDatabase() {

  if (
    currentRecordingId.length() == 0
  ) {

    Serial.println(
      "No recording UUID available."
    );

    return false;
  }


  if (
    !ensureWiFi()
  ) {

    Serial.println(
      "Cannot stop recording: WiFi unavailable."
    );

    return false;
  }


  Serial.println();

  Serial.println(
    "SUPABASE: STOPPING RECORDING..."
  );


  // ===================================================
  // CALCULATE DURATION
  // ===================================================

  unsigned long elapsedMilliseconds =
    millis() - recordingStartMillis;


  int durationSeconds =
    elapsedMilliseconds / 1000;


  Serial.print(
    "Duration seconds: "
  );

  Serial.println(
    durationSeconds
  );


  // ===================================================
  // SUPABASE PATCH ENDPOINT
  // ===================================================

  String endpoint =
    String(SUPABASE_URL) +
    "/rest/v1/recordings?id=eq." +
    currentRecordingId;


  HTTPClient http;


  http.begin(
    endpoint
  );


  http.addHeader(
    "Content-Type",
    "application/json"
  );


  http.addHeader(
    "apikey",
    SUPABASE_KEY
  );


  http.addHeader(
    "Authorization",
    String("Bearer ") +
    SUPABASE_KEY
  );


  // Return updated row for confirmation

  http.addHeader(
    "Prefer",
    "return=representation"
  );


  // ===================================================
  // IMPORTANT
  //
  // Do NOT send:
  // "ended_at":"now"
  //
  // The database trigger will automatically set ended_at.
  // ===================================================

  String payload =
    "{";


  payload +=
    "\"duration_seconds\":";

  payload +=
    String(
      durationSeconds
    );

  payload +=
    ",";


  payload +=
    "\"status\":\"pending_assessment\"";


  payload +=
    "}";


  Serial.println(
    "PATCH:"
  );

  Serial.println(
    endpoint
  );


  Serial.println(
    "Payload:"
  );

  Serial.println(
    payload
  );


  // ===================================================
  // SEND PATCH REQUEST
  // ===================================================

  int httpCode =
    http.sendRequest(
      "PATCH",
      payload
    );


  String response =
    http.getString();


  Serial.print(
    "HTTP Code: "
  );

  Serial.println(
    httpCode
  );


  Serial.print(
    "Response: "
  );

  Serial.println(
    response
  );


  http.end();


  // ===================================================
  // CHECK RESULT
  // ===================================================

  if (
    httpCode >= 200 &&
    httpCode < 300
  ) {

    Serial.println(
      "SUPABASE: RECORDING STOPPED!"
    );

    Serial.println(
      "ended_at updated."
    );

    Serial.println(
      "duration_seconds updated."
    );

    Serial.println(
      "status changed to pending_assessment."
    );


    return true;
  }


  Serial.println(
    "SUPABASE: FAILED TO STOP RECORDING!"
  );


  return false;
}


// =====================================================
// HANDLE START COMMAND
// =====================================================

void handleStartCommand() {

  Serial.println();

  Serial.println(
    "***** START COMMAND RECEIVED *****"
  );


  if (
    isRecordingActive
  ) {

    Serial.println(
      "Duplicate START ignored."
    );

    return;
  }


  bool created =
    createRecordingInDatabase();


  if (
    !created
  ) {

    Serial.println(
      "Database recording creation failed."
    );

    return;
  }


  // Start successful

  isRecordingActive =
    true;


  recordingStartMillis =
    millis();


  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    "RECORDING SESSION STARTED"
  );

  Serial.print(
    "Recording UUID: "
  );

  Serial.println(
    currentRecordingId
  );

  Serial.println(
    "================================"
  );
}


// =====================================================
// HANDLE STOP COMMAND
// =====================================================

void handleStopCommand() {

  Serial.println();

  Serial.println(
    "***** STOP COMMAND RECEIVED *****"
  );


  if (
    !isRecordingActive
  ) {

    Serial.println(
      "No active recording."
    );

    return;
  }


  bool stopped =
    stopRecordingInDatabase();


  if (
    !stopped
  ) {

    Serial.println(
      "Database stop failed."
    );

    return;
  }


  isRecordingActive =
    false;


  Serial.println(
    "RECORDING SESSION STOPPED"
  );


  currentRecordingId =
    "";
}


// =====================================================
// PARSE DATA PACKET
// =====================================================

bool parseDataPacket(
  String message
) {

  String packetValue =
    getPacketValue(
      message,
      "PACKET:"
    );


  String averageCO2Value =
    getPacketValue(
      message,
      "AVG_CO2:"
    );


  String averageTempValue =
    getPacketValue(
      message,
      "AVG_TEMP:"
    );


  String averageNoiseValue =
    getPacketValue(
      message,
      "AVG_NOISE:"
    );


  if (
    packetValue.length() == 0 ||
    averageCO2Value.length() == 0 ||
    averageTempValue.length() == 0 ||
    averageNoiseValue.length() == 0
  ) {

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
  String message
) {

  if (
    !message.startsWith(
      "NODE:1,"
    )
  ) {

    Serial.println(
      "Rejected: Not from Node 1."
    );

    return;
  }


  String packetType =
    getPacketValue(
      message,
      "TYPE:"
    );


  Serial.print(
    "Packet TYPE: "
  );

  Serial.println(
    packetType
  );


  // START

  if (
    packetType == "START"
  ) {

    handleStartCommand();

    return;
  }


  // STOP

  if (
    packetType == "STOP"
  ) {

    handleStopCommand();

    return;
  }


  // DATA

  if (
    packetType == "DATA"
  ) {

    if (
      !isRecordingActive
    ) {

      Serial.println(
        "DATA ignored: No active recording."
      );

      return;
    }


    bool valid =
      parseDataPacket(
        message
      );


    if (
      !valid
    ) {

      Serial.println(
        "Invalid DATA packet."
      );

      return;
    }


    Serial.println(
      "VALID DATA RECEIVED"
    );


    Serial.print(
      "CO2: "
    );

    Serial.println(
      receivedAverageCO2
    );


    Serial.print(
      "Temperature: "
    );

    Serial.println(
      receivedAverageTemp
    );


    Serial.print(
      "Noise: "
    );

    Serial.println(
      receivedAverageNoise
    );


    uploadEnvironmentalData();


    return;
  }


  Serial.println(
    "Unknown packet TYPE."
  );
}


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(
    115200
  );


  delay(2000);


  pinMode(
    LED_PIN,
    OUTPUT
  );


  digitalWrite(
    LED_PIN,
    LOW
  );


  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    "BANHA NODE 2"
  );

  Serial.println(
    "LORA + SUPABASE"
  );

  Serial.println(
    "================================"
  );


  // WIFI

  connectWiFi();


  // LORA SPI

  SPI.begin(
    LORA_SCK,
    LORA_MISO,
    LORA_MOSI,
    LORA_SS
  );


  LoRa.setPins(
    LORA_SS,
    LORA_RST,
    LORA_DIO0
  );


  Serial.println(
    "Starting LoRa..."
  );


  if (
    !LoRa.begin(
      LORA_BAND
    )
  ) {

    Serial.println(
      "LoRa FAILED!"
    );


    while (true) {

      digitalWrite(
        LED_PIN,
        HIGH
      );

      delay(200);


      digitalWrite(
        LED_PIN,
        LOW
      );

      delay(200);
    }
  }


  Serial.println(
    "LoRa Ready!"
  );

  Serial.println(
    "Node 2 ready."
  );
}


// =====================================================
// LOOP
// =====================================================

void loop() {


  // Reconnect WiFi if needed

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    connectWiFi();
  }


  // Check LoRa

  int packetSize =
    LoRa.parsePacket();


  if (
    packetSize <= 0
  ) {

    delay(10);

    return;
  }


  String receivedMessage =
    "";


  while (
    LoRa.available()
  ) {

    char c =
      (char)LoRa.read();


    receivedMessage +=
      c;
  }


  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.print(
    "LoRa: "
  );

  Serial.println(
    receivedMessage
  );


  handleLoRaMessage(
    receivedMessage
  );


  Serial.print(
    "RSSI: "
  );

  Serial.println(
    LoRa.packetRssi()
  );


  Serial.println(
    "================================"
  );


  digitalWrite(
    LED_PIN,
    HIGH
  );

  delay(100);

  digitalWrite(
    LED_PIN,
    LOW
  );
}