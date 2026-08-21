/*
   BANHA LoRa Node 1
   SENDER / TRANSMITTER

   RA-02 SX1278 Wiring:

   VCC   -> 3.3V
   GND   -> GND
   SCK   -> GPIO18
   MISO  -> GPIO19
   MOSI  -> GPIO23
   NSS   -> GPIO13
   RESET -> GPIO32
   DIO0  -> GPIO34
*/

#include <SPI.h>
#include <LoRa.h>

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
// PACKET COUNTER
// =====================================================

int packetNumber = 0;


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println("   BANHA LORA NODE 1 - SENDER");
  Serial.println("================================");


  // Start SPI using your pins
  SPI.begin(
    LORA_SCK,
    LORA_MISO,
    LORA_MOSI,
    LORA_SS);


  // Set LoRa pins
  LoRa.setPins(
    LORA_SS,
    LORA_RST,
    LORA_DIO0);


  Serial.println("Starting LoRa...");


  // Start LoRa
  if (!LoRa.begin(LORA_BAND)) {

    Serial.println("LoRa initialization FAILED!");

    while (true) {
      delay(1000);
    }
  }


  Serial.println("LoRa Ready!");
  Serial.println("Node 1 is ready to send.");
}


// =====================================================
// LOOP
// =====================================================

void loop() {

  packetNumber++;


  // ---------------------------------------------------
  // CREATE PACKET
  // ---------------------------------------------------

  String message = "Hello from Node 1";


  // ---------------------------------------------------
  // SEND PACKET
  // ---------------------------------------------------

  Serial.println("--------------------------------");

  Serial.print("Sending packet #");
  Serial.println(packetNumber);

  Serial.print("Message: ");
  Serial.println(message);


  LoRa.beginPacket();

  LoRa.print("NODE:1,");

  LoRa.print("PACKET:");
  LoRa.print(packetNumber);

  LoRa.print(",DATA:");
  LoRa.print(message);

  LoRa.endPacket();


  Serial.println("Packet sent successfully!");

  Serial.println("--------------------------------");


  // Send every 3 seconds
  delay(3000);
}