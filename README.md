# ESP32 38-Pin Pinout for BANHA

## TFT Display (ILI9341)

| TFT Pin | ESP32  |
| ------- | ------ |
| VCC     | 5V     |
| GND     | GND    |
| LED     | 5V     |
| CS      | GPIO15 |
| RESET   | GPIO2  |
| DC      | GPIO4  |
| SCK     | GPIO18 |
| MOSI    | GPIO23 |
| MISO    | GPIO19 |

---

## Touch (XPT2046)

| Touch Pin | ESP32  |
| --------- | ------ |
| T_CLK     | GPIO14 |
| T_DIN     | GPIO26 |
| T_DO      | GPIO25 |
| T_CS      | GPIO27 |
| T_IRQ     | GPIO33 |

---

## SCD41

| SCD41 Pin | ESP32  |
| --------- | ------ |
| VIN       | 3.3V   |
| GND       | GND    |
| SDA       | GPIO21 |
| SCL       | GPIO22 |

---

## INMP441 (I²S Microphone)

| INMP441 Pin | ESP32                |
| ----------- | -------------------- |
| VDD         | 3.3V                 |
| GND         | GND                  |
| L/R         | GND *(Left channel)* |
| WS (LRCLK)  | GPIO16               |
| SCK (BCLK)  | GPIO17               |
| SD (DOUT)   | GPIO5                |

The ESP32 allows the I²S signals (BCLK, LRCLK, and data) to be mapped to almost any suitable GPIO, so using GPIO16, GPIO17, and GPIO5 is a valid configuration. ([ShillehTek][1])

---

# Complete GPIO Usage

| GPIO   | Device  | Function      |
| ------ | ------- | ------------- |
| GPIO2  | TFT     | RESET         |
| GPIO4  | TFT     | DC            |
| GPIO5  | INMP441 | SD (Data Out) |
| GPIO14 | Touch   | CLK           |
| GPIO15 | TFT     | CS            |
| GPIO16 | INMP441 | WS (LRCLK)    |
| GPIO17 | INMP441 | SCK (BCLK)    |
| GPIO18 | TFT     | SCK           |
| GPIO19 | TFT     | MISO          |
| GPIO21 | SCD41   | SDA           |
| GPIO22 | SCD41   | SCL           |
| GPIO23 | TFT     | MOSI          |
| GPIO25 | Touch   | DO            |
| GPIO26 | Touch   | DIN           |
| GPIO27 | Touch   | CS            |
| GPIO33 | Touch   | IRQ           |

---

## Remaining Free GPIOs

You still have:

* GPIO12 *(boot strapping pin; avoid if possible)*
* GPIO13
* GPIO32
* GPIO34 *(input only)*
* GPIO35 *(input only)*
* GPIO36 *(input only)*
* GPIO39 *(input only)*

### This layout is well organized:

* **Right side:** TFT Display
* **Left side:** Touch Controller
* **Top:** SCD41
* **Bottom/remaining pins:** INMP441
