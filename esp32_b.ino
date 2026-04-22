/*
 * ARIA — ESP32_B (MOUTH)
 * Responsibilities:
 *   - Poll Flask server for audio response
 *   - Play TTS audio via MAX98357 amplifier + speaker
 *   - Display conversation text on OLED display
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "driver/i2s.h"

// ================= WIFI =================
const char* ssid     = "ESPTEST";
const char* password = "12345678";

// ================= SERVER =================
const char* statusURL = "http://10.84.51.142:5000/status";
const char* audioURL  = "http://10.84.51.142:5000/get-audio-response";

// ================= I2S SPEAKER (MAX98357) =================
#define I2S_BCLK   26
#define I2S_LRCLK  25
#define I2S_DOUT   27

// ================= OLED =================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire);

// ================= OLED HELPER =================
void showOLED(String line1, String line2 = "", String line3 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(line1);
  display.println(line2);
  display.println(line3);
  display.display();
}

// Wrap long text across OLED lines
void showResponse(String text) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.setTextWrap(true);
  // Show first 128 chars (OLED limit)
  display.println(text.substring(0, 128));
  display.display();
}

// ================= I2S SPEAKER SETUP =================
void i2s_speaker_install() {
  i2s_config_t config = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = 16000,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 1024,
    .use_apll             = false,
    .tx_desc_auto_clear   = true,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_BCLK,
    .ws_io_num    = I2S_LRCLK,
    .data_out_num = I2S_DOUT,
    .data_in_num  = -1
  };
  i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_set_clk(I2S_NUM_0, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  // OLED init
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED failed");
    for (;;);
  }
  showOLED("ARIA Mouth", "Booting...");

  // WiFi connect
  WiFi.begin(ssid, password);
  showOLED("Connecting WiFi...");
  while (WiFi.status() != WL_CONNECTED) delay(500);

  showOLED("WiFi OK", WiFi.localIP().toString());
  Serial.println("Mouth IP: " + WiFi.localIP().toString());

  // I2S speaker
  i2s_speaker_install();

  showOLED("ARIA READY", "Waiting...");
}

// ================= LOOP =================
void loop() {
  HTTPClient http;

  // Poll /status
  http.begin(statusURL);
  int statusCode = http.GET();

  if (statusCode == 200) {
    String payload = http.getString();

    if (payload.indexOf("true") != -1) {
      http.end();

      showOLED("ARIA SPEAKING", "Playing...");

      // Fetch audio
      http.begin(audioURL);
      int audioCode = http.GET();

      if (audioCode == 200) {
        WiFiClient* stream = http.getStreamPtr();
        uint8_t buffer[1024];
        size_t written;

        // Stream audio chunk by chunk to speaker
        while (http.connected()) {
          int available = stream->available();
          if (available > 0) {
            int r = stream->readBytes(buffer, min(available, 1024));
            i2s_write(I2S_NUM_0, buffer, r, &written, portMAX_DELAY);
          }
          delay(1);
        }

        showOLED("ARIA READY", "Waiting...");
        delay(2000);
      } else {
        showOLED("Audio fetch", "failed: " + String(audioCode));
      }
    }
  }

  http.end();
  delay(1000); // poll every 1 second
}
