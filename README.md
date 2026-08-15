# ESP32 38-Pin Pinout for BANHA

## TFT Display (ILI9341)

| TFT Pin | ESP32  | Note                 |
| ------- | ------ | -------------------- |
| VCC     | 5V*    |                      |
| GND     | GND    |                      |
| LED     | 5V*    |                      |
| CS      | GPIO15 | TFT only             |
| RESET   | GPIO2  | TFT only             |
| DC      | GPIO4  | TFT only             |
| SCK     | GPIO18 | **Shared with LoRa** |
| MOSI    | GPIO23 | **Shared with LoRa** |
| MISO    | GPIO19 | **Shared with LoRa** |

---

## Touch (XPT2046)

| Touch Pin | ESP32  | Note       |
| --------- | ------ | ---------- |
| T_CLK     | GPIO14 | Touch only |
| T_DIN     | GPIO26 | Touch only |
| T_DO      | GPIO25 | Touch only |
| T_CS      | GPIO27 | Touch only |
| T_IRQ     | GPIO33 | Touch only |

---

## SCD41

| SCD41 Pin | ESP32  | Note       |
| --------- | ------ | ---------- |
| VIN       | 3.3V   |            |
| GND       | GND    |            |
| SDA       | GPIO22 | SCD41 only |
| SCL       | GPIO21 | SCD41 only |

---

## INMP441 (I²S Microphone)

| INMP441 Pin | ESP32  | Note            |
| ----------- | ------ | --------------- |
| VDD         | 3.3V   |                 |
| GND         | GND    |                 |
| L/R         | GND    | Left channel    |
| WS (LRCLK)  | GPIO16 | Microphone only |
| SCK (BCLK)  | GPIO17 | Microphone only |
| SD (DOUT)   | GPIO5  | Microphone only |

---

## LoRa RA-02 (SX1278)

| RA-02 Pin | ESP32  | Note                |
| --------- | ------ | ------------------- |
| VCC       | 3.3V   |                     |
| GND       | GND    |                     |
| SCK       | GPIO18 | **Shared with TFT** |
| MISO      | GPIO19 | **Shared with TFT** |
| MOSI      | GPIO23 | **Shared with TFT** |
| NSS / CS  | GPIO13 | **LoRa only**       |
| RESET     | GPIO32 | LoRa only           |
| DIO0      | GPIO34 | LoRa only           |

The shared SPI arrangement is intentional: SPI devices can share **SCK, MOSI, and MISO**, while each device has its own CS line. ([Espressif Systems][1])

---

# Complete GPIO Usage

|       GPIO | Device         | Function     | Status        |
| ---------: | -------------- | ------------ | ------------- |
|  **GPIO2** | TFT            | RESET        | TFT only      |
|  **GPIO4** | TFT            | DC           | TFT only      |
|  **GPIO5** | INMP441        | SD / DOUT    | Mic only      |
| **GPIO13** | LoRa           | NSS / CS     | LoRa only     |
| **GPIO14** | Touch          | T_CLK        | Touch only    |
| **GPIO15** | TFT            | CS           | TFT only      |
| **GPIO16** | INMP441        | WS / LRCLK   | Mic only      |
| **GPIO17** | INMP441        | SCK / BCLK   | Mic only      |
| **GPIO18** | **TFT + LoRa** | **SPI SCK**  | 🔗 **SHARED** |
| **GPIO19** | **TFT + LoRa** | **SPI MISO** | 🔗 **SHARED** |
| **GPIO21** | SCD41          | SDA          | SCD41 only    |
| **GPIO22** | SCD41          | SCL          | SCD41 only    |
| **GPIO23** | **TFT + LoRa** | **SPI MOSI** | 🔗 **SHARED** |
| **GPIO25** | Touch          | T_DO / MISO  | Touch only    |
| **GPIO26** | Touch          | T_DIN / MOSI | Touch only    |
| **GPIO27** | Touch          | T_CS         | Touch only    |
| **GPIO32** | LoRa           | RESET        | LoRa only     |
| **GPIO33** | Touch          | T_IRQ        | Touch only    |
| **GPIO34** | LoRa           | DIO0         | LoRa only     |

### 🔗 Shared pins

Only **3 GPIOs are shared**:

```text
GPIO18 → TFT SCK  + LoRa SCK
GPIO19 → TFT MISO + LoRa MISO
GPIO23 → TFT MOSI + LoRa MOSI
```

The **CS pins are NOT shared**:

```text
GPIO15 → TFT CS
GPIO13 → LoRa NSS/CS
```

[1]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/spi_slave.html?utm_source=chatgpt.com "SPI Slave Driver - ESP32 - — ESP-IDF Programming Guide v6.0.2 documentation"
