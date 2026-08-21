/*
   ====================================================
   BANHA LoRa Node 2
   RECEIVER
   ====================================================

   Receives 1-minute averaged environmental data
   from BANHA Node 1.

   Expected packet:

   NODE:1,PACKET:1,AVG_CO2:842.5,AVG_TEMP:29.8,AVG_NOISE:51.8

   RA-02 SX1278 WIRING
   ====================================================

   VCC   -> 3.3V
   GND   -> GND

   SCK   -> GPIO18
   MISO  -> GPIO19
   MOSI  -> GPIO23

   NSS   -> GPIO13
   RESET -> GPIO32
   DIO0  -> GPIO34

   Frequency:
   433 MHz
*/


#include <SPI.h>
#include <LoRa.h>


// =====================================================
// BUILT-IN LED
// =====================================================

#define LED_PIN 2


// =====================================================
// LORA PINS
// =====================================================

#define LORA_SCK 18
#define LORA_MISO 19
#define LORA_MOSI 23

#define LORA_SS 13
#define LORA_RST 32
#define LORA_DIO0 34


// =====================================================
// LORA FREQUENCY
// =====================================================

#define LORA_BAND 433E6


// =====================================================
// RECEIVED DATA
// =====================================================

unsigned long receivedPacketNumber = 0;

float receivedAverageCO2 = 0.0f;

float receivedAverageTemp = 0.0f;

float receivedAverageNoise = 0.0f;


// =====================================================
// GET VALUE FROM PACKET
// =====================================================

String getPacketValue(
  String message,
  String key) {

  int startIndex =
    message.indexOf(key);


  // Key not found

  if (startIndex == -1) {

    return "";
  }


  // Move past key

  startIndex += key.length();


  // Find next comma

  int endIndex =
    message.indexOf(
      ",",
      startIndex);


  // Last value in packet

  if (endIndex == -1) {

    endIndex =
      message.length();
  }


  return message.substring(
    startIndex,
    endIndex);
}


// =====================================================
// PARSE BANHA 1-MINUTE PACKET
// =====================================================

bool parseBANHAPacket(
  String message) {


  // ===================================================
  // CHECK NODE
  // ===================================================

  if (!message.startsWith("NODE:1,")) {

    Serial.println(
      "Invalid packet: Not from Node 1.");

    return false;
  }


  // ===================================================
  // EXTRACT VALUES
  // ===================================================

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


  // ===================================================
  // VALIDATE
  // ===================================================

  if (
    packetValue.length() == 0 || averageCO2Value.length() == 0 || averageTempValue.length() == 0 || averageNoiseValue.length() == 0) {

    Serial.println(
      "Invalid BANHA packet: Missing average data.");

    return false;
  }


  // ===================================================
  // CONVERT
  // ===================================================

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
// PRINT RECEIVED DATA
// =====================================================

void printReceivedData() {

  Serial.println(
    "--------------------------------");

  Serial.println(
    "RECEIVED BANHA 1-MINUTE AVERAGE");

  Serial.println(
    "--------------------------------");


  Serial.print(
    "Packet: ");

  Serial.println(
    receivedPacketNumber);


  Serial.print(
    "Average CO2   : ");

  Serial.print(
    receivedAverageCO2,
    1);

  Serial.println(
    " ppm");


  Serial.print(
    "Average Temp  : ");

  Serial.print(
    receivedAverageTemp,
    1);

  Serial.println(
    " C");


  Serial.print(
    "Average Noise : ");

  Serial.print(
    receivedAverageNoise,
    1);

  Serial.println(
    " dB");


  Serial.println(
    "--------------------------------");
}


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(
    115200);


  delay(2000);


  // ===================================================
  // LED
  // ===================================================

  pinMode(
    LED_PIN,
    OUTPUT);

  digitalWrite(
    LED_PIN,
    LOW);


  // ===================================================
  // HEADER
  // ===================================================

  Serial.println();

  Serial.println(
    "================================");

  Serial.println(
    " BANHA LORA NODE 2 - RECEIVER");

  Serial.println(
    "================================");


  // ===================================================
  // SPI
  // ===================================================

  SPI.begin(
    LORA_SCK,
    LORA_MISO,
    LORA_MOSI,
    LORA_SS);


  // ===================================================
  // LORA PINS
  // ===================================================

  LoRa.setPins(
    LORA_SS,
    LORA_RST,
    LORA_DIO0);


  // ===================================================
  // START LORA
  // ===================================================

  Serial.println(
    "Starting LoRa...");


  if (!LoRa.begin(LORA_BAND)) {

    Serial.println(
      "LoRa initialization FAILED!");

    Serial.println(
      "Check RA-02 wiring and power.");


    while (true) {

      digitalWrite(
        LED_PIN,
        HIGH);

      delay(200);

      digitalWrite(
        LED_PIN,
        LOW);

      delay(200);
    }
  }


  // ===================================================
  // READY
  // ===================================================

  Serial.println(
    "LoRa Ready!");

  Serial.println(
    "Frequency: 433 MHz");

  Serial.println(
    "Waiting for 1-minute averages from Node 1...");

  Serial.println();

  Serial.println(
    "Expected packet:");

  Serial.println(
    "NODE:1,PACKET:1,AVG_CO2:842.5,AVG_TEMP:29.8,AVG_NOISE:51.8");
}


// =====================================================
// LOOP
// =====================================================

void loop() {


  // ===================================================
  // CHECK FOR PACKET
  // ===================================================

  int packetSize =
    LoRa.parsePacket();


  if (packetSize <= 0) {

    return;
  }


  Serial.println();

  Serial.println(
    "================================");


  Serial.print(
    "Packet detected. Size: ");

  Serial.println(
    packetSize);


  // ===================================================
  // PACKET SIZE VALIDATION
  // ===================================================

  /*
     Example packet is approximately:

     NODE:1,PACKET:1,
     AVG_CO2:842.5,
     AVG_TEMP:29.8,
     AVG_NOISE:51.8
  */

  if (packetSize > 150) {

    Serial.println(
      "Ignored: Suspicious packet size.");


    while (LoRa.available()) {

      LoRa.read();
    }


    Serial.println(
      "================================");

    return;
  }


  // ===================================================
  // READ PACKET
  // ===================================================

  String receivedMessage = "";


  while (LoRa.available()) {

    char c =
      (char)LoRa.read();


    receivedMessage += c;
  }


  // ===================================================
  // SHOW RAW DATA
  // ===================================================

  Serial.print(
    "Raw data: ");

  Serial.println(
    receivedMessage);


  // ===================================================
  // PARSE
  // ===================================================

  bool validPacket =
    parseBANHAPacket(
      receivedMessage);


  if (!validPacket) {

    Serial.println(
      "Packet rejected.");

    Serial.println(
      "================================");

    return;
  }


  // ===================================================
  // VALID PACKET
  // ===================================================

  Serial.println(
    "***** VALID NODE 1 BANHA PACKET *****");


  // ===================================================
  // DISPLAY DATA
  // ===================================================

  printReceivedData();


  // ===================================================
  // SIGNAL QUALITY
  // ===================================================

  Serial.print(
    "RSSI: ");

  Serial.print(
    LoRa.packetRssi());

  Serial.println(
    " dBm");


  Serial.print(
    "SNR: ");

  Serial.print(
    LoRa.packetSnr());

  Serial.println(
    " dB");


  // ===================================================
  // BLINK LED
  // ===================================================

  digitalWrite(
    LED_PIN,
    HIGH);

  delay(500);

  digitalWrite(
    LED_PIN,
    LOW);


  Serial.println(
    "LED blinked - 1-minute average accepted!");


  Serial.println(
    "================================");
}