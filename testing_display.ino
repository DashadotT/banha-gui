/*
  testing_display.ino
  ESP32 + ILI9341 3.2" SPI (with XPT2046 resistive touch) running an
  EEZ Studio LVGL v8 UI (main_screen / selection_screen / recording_screen).

  BEFORE UPLOADING, install these libraries via Library Manager:
    - TFT_eSPI            (Bodmer)      -> must be configured, see notes below
    - lvgl                 version 8.4.x (NOT v9 - this project was exported for v8)
    - XPT2046_Touchscreen  (Paul Stoffregen)

  TFT_eSPI CONFIGURATION (one-time, required):
    TFT_eSPI is configured at compile time via a "User_Setup.h" file inside
    the library folder itself (Arduino/libraries/TFT_eSPI/User_Setup.h),
    not in this sketch. Open that file and set it up for ILI9341 + ESP32, e.g.:

      #define ILI9341_DRIVER
      #define TFT_MISO 19
      #define TFT_MOSI 23
      #define TFT_SCLK 18
      #define TFT_CS   15
      #define TFT_DC    2
      #define TFT_RST   4
      #define SPI_FREQUENCY  40000000

    Adjust the pin numbers to match how your ILI9341 module is actually wired
    to your ESP32 dev board. If you tell me your exact wiring I can give you
    the exact pins instead of these typical defaults.

  TOUCH WIRING (XPT2046, shares the SPI bus with the display, separate CS):
    T_CLK -> same as TFT_SCLK (18)
    T_DIN -> same as TFT_MOSI (23)
    T_DO  -> same as TFT_MISO (19)
    T_CS  -> a free GPIO, set XPT2046_CS below (default 21)
    T_IRQ -> optional, not required for polling mode
*/

#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>

#include "ui.h"
#include "vars.h"

// ---------- Display ----------
static const uint16_t SCREEN_W = 320;
static const uint16_t SCREEN_H = 240;

TFT_eSPI tft = TFT_eSPI();

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[SCREEN_W * 20]; // partial buffer, 20 lines tall

// ---------- Touch ----------
#define XPT2046_CS 21
XPT2046_Touchscreen ts(XPT2046_CS);

// Raw touch calibration - adjust these if touch coordinates feel off/inverted
static const int TOUCH_MIN_X = 200;
static const int TOUCH_MAX_X = 3700;
static const int TOUCH_MIN_Y = 240;
static const int TOUCH_MAX_Y = 3800;

static void disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

static void touch_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data) {
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    int x = map(p.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, SCREEN_W);
    int y = map(p.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, SCREEN_H);
    x = constrain(x, 0, SCREEN_W - 1);
    y = constrain(y, 0, SCREEN_H - 1);

    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// ---------- Fake sensor data (replace with real sensor reads) ----------
static uint32_t last_sensor_update = 0;

static void update_demo_sensor_values() {
  if (millis() - last_sensor_update < 2000) return;
  last_sensor_update = millis();

  float temp = 20.0f + (random(0, 100) / 10.0f); // 20.0 - 30.0
  int32_t co2 = 400 + random(0, 600);            // 400 - 1000
  int32_t noise = 30 + random(0, 50);             // 30 - 80

  set_var_temp_value(temp);
  set_var_temp_status(temp > 27.0f ? "High" : "Normal");

  set_var_co2_value(co2);
  set_var_co2_status(co2 > 800 ? "High" : "Normal");

  set_var_noise_value(noise);
  set_var_noise_status(noise > 65 ? "High" : "Normal");
}

void setup() {
  Serial.begin(115200);

  // Display
  tft.begin();
  tft.setRotation(1); // landscape, 320x240 - change to 3 if image is upside down
  tft.fillScreen(TFT_BLACK);

  // Touch
  ts.begin();
  ts.setRotation(1);

  // LVGL
  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SCREEN_W * 20);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_W;
  disp_drv.ver_res = SCREEN_H;
  disp_drv.flush_cb = disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read;
  lv_indev_drv_register(&indev_drv);

  // EEZ Studio generated UI
  ui_init();

  Serial.println("UI init complete");
}

void loop() {
  lv_tick_inc(5);
  lv_timer_handler();
  ui_tick();
  update_demo_sensor_values();
  delay(5);
}
