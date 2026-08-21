/*
   BANHA LoRa Node 2 - RECEIVER
   Only accepts valid packets from Node 1

   Expected packet format:

   NODE:1,PACKET:1,DATA:Hello from Node 1
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

#define LORA_BAND 433E6


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);
  delay(2000);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println();
  Serial.println("================================");
  Serial.println(" BANHA LORA NODE 2 - RECEIVER");
  Serial.println("================================");

  SPI.begin(
    LORA_SCK,
    LORA_MISO,
    LORA_MOSI,
    LORA_SS);

  LoRa.setPins(
    LORA_SS,
    LORA_RST,
    LORA_DIO0);

  Serial.println("Starting LoRa...");

  if (!LoRa.begin(LORA_BAND)) {

    Serial.println("LoRa initialization FAILED!");

    while (true) {

      digitalWrite(LED_PIN, HIGH);
      delay(200);

      digitalWrite(LED_PIN, LOW);
      delay(200);
    }
  }

  Serial.println("LoRa Ready!");
  Serial.println("Waiting for valid Node 1 packets...");
}


// =====================================================
// LOOP
// =====================================================

void loop() {

  int packetSize = LoRa.parsePacket();

  if (packetSize <= 0) {
    return;
  }

  Serial.println();
  Serial.println("================================");

  Serial.print("Packet detected. Size: ");
  Serial.println(packetSize);

  // Reject suspicious packet sizes.
  // Our BANHA test packets are much smaller.
  if (packetSize > 100) {

    Serial.println("Ignored: Suspicious packet size.");

    // Clear received bytes
    while (LoRa.available()) {
      LoRa.read();
    }

    Serial.println("================================");

    return;
  }


  // Read received packet
  String receivedMessage = "";

  while (LoRa.available()) {

    char c = (char)LoRa.read();

    receivedMessage += c;
  }


  Serial.print("Raw data: ");
  Serial.println(receivedMessage);


  // Accept only BANHA Node 1 packets
  if (!receivedMessage.startsWith("NODE:1,")) {

    Serial.println("Ignored: Not a BANHA Node 1 packet.");

    Serial.println("================================");

    return;
  }


  // Valid packet
  Serial.println("***** VALID NODE 1 PACKET *****");

  Serial.print("Message: ");
  Serial.println(receivedMessage);

  Serial.print("RSSI: ");
  Serial.print(LoRa.packetRssi());
  Serial.println(" dBm");

  Serial.print("SNR: ");
  Serial.print(LoRa.packetSnr());
  Serial.println(" dB");


  // Light Node 2 built-in LED
  digitalWrite(LED_PIN, HIGH);

  delay(500);

  digitalWrite(LED_PIN, LOW);

  Serial.println("LED blinked - Packet accepted!");
  Serial.println("================================");
}