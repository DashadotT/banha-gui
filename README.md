# BANHA Node 1 — Complete Wiring List (Final)

## TFT Display (ILI9341)

| TFT Pin | ESP32 Pin | Note |
|---|---|---|
| VCC | 5V* | |
| GND | GND | |
| LED | 5V* | |
| CS | GPIO15 | TFT only |
| RESET | GPIO2 | TFT only |
| DC | GPIO4 | TFT only |
| SCK | GPIO18 | TFT only |
| MOSI | GPIO23 | TFT only |
| MISO | — | No connection |

## Touch (XPT2046)

| Touch Pin | ESP32 Pin | Note |
|---|---|---|
| T_CLK | GPIO14 | Shared with LoRa SCK |
| T_DIN | GPIO26 | Shared with LoRa MOSI |
| T_DO | GPIO25 | Shared with LoRa MISO |
| T_CS | GPIO27 | Touch only |
| T_IRQ | GPIO33 | Touch only |

## SCD41 (CO₂ / Temp Sensor)

| SCD41 Pin | ESP32 Pin | Note |
|---|---|---|
| VIN | 5V | |
| GND | GND | |
| SDA | GPIO22 | SCD41 only |
| SCL | GPIO21 | SCD41 only |

## INMP441 (I²S Microphone)

| INMP441 Pin | ESP32 Pin | Note |
|---|---|---|
| VDD | 3.3V | |
| GND | GND | |
| L/R | GND | Left channel select |
| WS (LRCLK) | GPIO16 | Microphone only |
| SCK (BCLK) | GPIO17 | Microphone only |
| SD (DOUT) | GPIO5 | Microphone only |

## LoRa RA-02 (SX1278) — Node 1

| RA-02 Pin | ESP32 Pin | Note |
|---|---|---|
| VCC | 3.3V | |
| GND | GND | |
| SCK | GPIO14 | Shared with Touch T_CLK |
| MISO | GPIO25 | Shared with Touch T_DO |
| MOSI | GPIO26 | Shared with Touch T_DIN |
| NSS / CS | GPIO13 | LoRa only |
| RESET | GPIO32 | LoRa only |
| DIO0 | GPIO34 | LoRa only |

## LoRa RA-02 (SX1278) node 2

| RA-02 Pin | ESP32  | Note                |
| --------- | ------ | ------------------- |
| VCC       | 3.3V   |                     |
| GND       | GND    |                     |
| SCK       | GPIO23 | LoRa only           |
| MISO      | GPIO19 | LoRa only           |
| MOSI      | GPIO17 | LoRa only           |
| NSS / CS  | GPIO16 | **LoRa only**       |
| RESET     | GPIO14 | LoRa only           |
| DIO0      | GPIO26 | LoRa only           |


## GPIO Usage Summary (Node 1)

| GPIO | Used By |
|---|---|
| 2 | TFT RESET |
| 4 | TFT DC |
| 5 | INMP441 SD |
| 13 | LoRa NSS |
| 14 | Touch T_CLK **+ LoRa SCK** (shared HSPI) |
| 15 | TFT CS |
| 16 | INMP441 WS |
| 17 | INMP441 SCK |
| 18 | TFT SCK (VSPI, dedicated) |
| 21 | SCD41 SCL |
| 22 | SCD41 SDA |
| 23 | TFT MOSI (VSPI, dedicated) |
| 25 | Touch T_DO **+ LoRa MISO** (shared HSPI) |
| 26 | Touch T_DIN **+ LoRa MOSI** (shared HSPI) |
| 27 | Touch T_CS |
| 32 | LoRa RESET |
| 33 | Touch T_IRQ |
| 34 | LoRa DIO0 |

No pin conflicts — GPIO14/25/26 are intentionally shared between Touch and LoRa (both libraries handle SPI transactions properly), and every other pin is dedicated to exactly one device.

**Node 2 (receiver)** is unchanged from your original wiring — it never shared pins with anything, so no updates needed there.


[1]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/spi_slave.html?utm_source=chatgpt.com "SPI Slave Driver - ESP32 - — ESP-IDF Programming Guide v6.0.2 documentation"
