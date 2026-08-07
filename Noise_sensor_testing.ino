#include <driver/i2s.h>
#include <math.h>

// =======================
// INMP441 Pins
// =======================
#define I2S_WS    16
#define I2S_SCK   17
#define I2S_SD    5
#define I2S_PORT  I2S_NUM_0

// Number of samples
#define SAMPLE_BUFFER_SIZE 1024

// =====================================================
// CALIBRATION
// Measure the RMS in a quiet classroom (~35 dB)
// Replace this value after calibration.
// =====================================================
const float QUIET_RMS = 185.0;   // <-- CHANGE THIS AFTER TESTING

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("==================================");
  Serial.println("   INMP441 Sound Level Meter");
  Serial.println("==================================");

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
  i2s_zero_dma_buffer(I2S_PORT);

  Serial.println("Microphone Ready");
}

void loop() {

  int32_t samples[SAMPLE_BUFFER_SIZE];
  size_t bytesRead = 0;

  i2s_read(
    I2S_PORT,
    samples,
    sizeof(samples),
    &bytesRead,
    portMAX_DELAY
  );

  int sampleCount = bytesRead / sizeof(int32_t);

  double sum = 0;

  for (int i = 0; i < sampleCount; i++) {

    // Convert 32-bit microphone data
    float sample = samples[i] >> 14;

    sum += sample * sample;
  }

  // RMS
  float rms = sqrt(sum / sampleCount);

  if (rms < 1)
    rms = 1;

  // Estimated dB
  float dB = 35.0 + 20.0 * log10(rms / QUIET_RMS);

  // Limit to realistic values
  if (dB < 0) dB = 0;
  if (dB > 120) dB = 120;

  Serial.print("RMS: ");
  Serial.print(rms, 1);

  Serial.print("\tEstimated dB: ");
  Serial.println(dB, 1);

  delay(200);
}