/*
 * ARIA — ESP32_A (EAR) — v3
 *
 * NEW FEATURES:
 * 1. Continuous Voice Activity Detection (VAD)
 *    - Mic always listens in background
 *    - Automatically triggers recording when voice is detected
 *    - No need to press button for wake word activation
 *    - Button still works as manual backup trigger
 *
 * 2. Deep Sleep / Light Sleep
 *    - After sending audio + receiving response, ESP32 enters light sleep
 *    - WiFi stays connected during light sleep (faster wake)
 *    - Wakes up automatically to listen again
 *    - Saves significant battery vs always-on
 *
 * HOW IT WORKS:
 *   [Idle] → mic samples audio → checks volume level
 *      ↓ voice detected (volume > VAD_THRESHOLD)
 *   [Recording] → captures 5 seconds → sends to server
 *      ↓ sent
 *   [Light Sleep] → 2 seconds rest → wake up
 *      ↓
 *   [Idle] → back to listening
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "driver/i2s.h"
#include "esp_sleep.h"

// ================= WIFI =================
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// ================= SERVER =================
const char* serverHost = "192.168.X.X";   // Your PC IP
const int   serverPort = 5000;
const char* sensorURL  = "http://192.168.X.X:5000/update-sensor";

// ================= WAKE WORD =================
const char* WAKE_WORD = "ARIA";   // Set your wake word here

// ================= PINS =================
#define BUTTON_PIN  4
#define LED_PIN     2

// INMP441 I2S Microphone
#define I2S_WS   25
#define I2S_SD   34
#define I2S_SCK  26

// ================= AUDIO =================
#define SAMPLE_RATE      16000
#define RECORD_SECONDS   8       // 8 seconds — enough for wake word + full question
#define BUFFER_SIZE      1024

// ================= VAD (Voice Activity Detection) =================
// How loud the mic needs to be before we start recording
// Increase if false triggers, decrease if mic doesn't pick up voice
#define VAD_THRESHOLD     500    // audio amplitude threshold (0-32767)
#define VAD_SAMPLE_COUNT  256    // number of samples to check per VAD pass
#define VAD_HOLD_MS       200    // reduced to 200ms for faster voice trigger

// ================= DEEP SLEEP =================
#define SLEEP_AFTER_RESPONSE_MS  2000   // light sleep duration after response (ms)

// ================= OLED =================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire);

// ================= STATE =================
bool recording           = false;
int  frontDist           = 999;
int  backDist            = 999;
unsigned long lastSensorPost  = 0;
unsigned long lastVoiceDetect = 0;
#define SENSOR_POST_INTERVAL 1000

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

// ================= I2S MIC SETUP =================
void i2s_install() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = BUFFER_SIZE,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_SCK,
    .ws_io_num    = I2S_WS,
    .data_out_num = -1,
    .data_in_num  = I2S_SD
  };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

// ================= VAD — CHECK IF VOICE IS PRESENT =================
bool isVoiceDetected() {
  int16_t samples[VAD_SAMPLE_COUNT];
  size_t bytesRead = 0;

  // Read a small chunk of audio
  i2s_read(I2S_NUM_0, samples, sizeof(samples), &bytesRead, 100);

  if (bytesRead == 0) return false;

  int count = bytesRead / 2;
  long sumAbs = 0;

  for (int i = 0; i < count; i++) {
    sumAbs += abs(samples[i]);
  }

  long avgAmplitude = sumAbs / count;

  // Debug: print amplitude to Serial Monitor to help tune VAD_THRESHOLD
  // Serial.println("Amplitude: " + String(avgAmplitude));

  return avgAmplitude > VAD_THRESHOLD;
}

// ================= RECORD + SEND =================
void recordAndSend() {
  recording = true;
  digitalWrite(LED_PIN, HIGH);
  showOLED("LISTENING...", "Speak now!");

  WiFiClient client;
  if (!client.connect(serverHost, serverPort)) {
    showOLED("SERVER ERROR", "Check IP/WiFi");
    recording = false;
    digitalWrite(LED_PIN, LOW);
    return;
  }

  int totalBytes = SAMPLE_RATE * 2 * RECORD_SECONDS;

  client.println("POST /process-audio HTTP/1.1");
  client.printf("Host: %s:%d\r\n", serverHost, serverPort);
  client.println("Content-Type: application/octet-stream");
  client.printf("X-Wake-Word: %s\r\n", WAKE_WORD);
  client.printf("Content-Length: %d\r\n\r\n", totalBytes);

  char buffer[BUFFER_SIZE];
  size_t bytesRead;
  int sent = 0;

  // Flush stale audio from buffer before recording
  i2s_read(I2S_NUM_0, buffer, BUFFER_SIZE, &bytesRead, portMAX_DELAY);

  while (sent < totalBytes) {
    i2s_read(I2S_NUM_0, buffer, BUFFER_SIZE, &bytesRead, portMAX_DELAY);
    if (bytesRead > 0) {
      client.write((uint8_t*)buffer, bytesRead);
      sent += bytesRead;
    }
  }

  client.stop();
  digitalWrite(LED_PIN, LOW);
  showOLED("THINKING...", "Processing...");
  recording = false;

  // Light sleep after sending — saves power while waiting for response
  enterLightSleep(SLEEP_AFTER_RESPONSE_MS);

  showOLED("ARIA READY", "Listening...");
}

// ================= LIGHT SLEEP =================
void enterLightSleep(int ms) {
  Serial.println("💤 Light sleep: " + String(ms) + "ms");
  // Light sleep keeps WiFi alive and wakes on timer
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
  esp_light_sleep_start();
  Serial.println("⚡ Woke from light sleep");
}

// ================= POST SENSOR DATA =================
void postSensorData() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(sensorURL);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"front\":" + String(frontDist) + ",\"back\":" + String(backDist) + "}";
  http.POST(body);
  http.end();
}

// ================= READ SERIAL FROM ARDUINO =================
void readArduinoSerial() {
  while (Serial2.available()) {
    String line = Serial2.readStringUntil('\n');
    line.trim();
    if (line.startsWith("DIST:")) {
      int fIdx = line.indexOf("F:");
      int bIdx = line.indexOf("B:");
      if (fIdx != -1 && bIdx != -1) {
        frontDist = line.substring(fIdx + 2, bIdx - 1).toInt();
        backDist  = line.substring(bIdx + 2).toInt();
      }
    }
  }
}

// ================= HTTP SERVER FOR MOTOR COMMANDS =================
WiFiServer motorServer(80);

void handleMotorServer() {
  WiFiClient client = motorServer.available();
  if (!client) return;

  String request = "";
  while (client.connected() && client.available()) {
    String line = client.readStringUntil('\n');
    request += line;
    if (line == "\r") break;
  }

  String body = "";
  while (client.available()) {
    body += (char)client.read();
  }

  String direction = "stop";
  String speed     = "normal";

  if (body.indexOf("\"forward\"") != -1)       direction = "forward";
  else if (body.indexOf("\"backward\"") != -1) direction = "backward";
  else if (body.indexOf("\"left\"") != -1)     direction = "left";
  else if (body.indexOf("\"right\"") != -1)    direction = "right";
  else if (body.indexOf("\"stop\"") != -1)     direction = "stop";

  if (body.indexOf("\"slow\"") != -1)          speed = "slow";
  else if (body.indexOf("\"fast\"") != -1)     speed = "fast";

  String cmd = direction + ":" + speed + "\n";
  Serial2.print(cmd);
  Serial.println("🤖 Motor → Arduino: " + cmd);
  showOLED("MOVING", direction, speed);

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Length: 2");
  client.println();
  client.println("OK");
  client.stop();
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, 16, 17);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED failed");
    for (;;);
  }
  showOLED("ARIA Booting...");

  WiFi.begin(ssid, password);
  showOLED("Connecting WiFi...");
  while (WiFi.status() != WL_CONNECTED) delay(300);

  showOLED("WiFi OK", WiFi.localIP().toString());
  Serial.println("ESP32_A (Ear) IP: " + WiFi.localIP().toString());

  i2s_install();
  motorServer.begin();

  showOLED("ARIA READY", "Listening...", "or press button");
  Serial.println("🎤 VAD active — speak to trigger recording");
  delay(500);
}

// ================= LOOP =================
void loop() {
  // Read sensor data from Arduino
  readArduinoSerial();

  // Post sensor data to Flask server every 1 second
  if (millis() - lastSensorPost > SENSOR_POST_INTERVAL) {
    postSensorData();
    lastSensorPost = millis();
  }

  // Handle motor commands from Flask server
  handleMotorServer();

  if (recording) return;

  // ---- MANUAL BUTTON TRIGGER ----
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      recordAndSend();
      while (digitalRead(BUTTON_PIN) == LOW) delay(10);
      return;
    }
  }

  // ---- AUTOMATIC VAD TRIGGER ----
  // Continuously sample mic — if voice detected, start recording
  if (isVoiceDetected()) {
    unsigned long now = millis();

    // Debounce — wait VAD_HOLD_MS of continuous voice before triggering
    if (lastVoiceDetect == 0) {
      lastVoiceDetect = now;
    }

    if (now - lastVoiceDetect >= VAD_HOLD_MS) {
      Serial.println("🎙️ Voice detected — auto-triggering recording!");
      showOLED("VOICE DETECTED", "Recording...");
      lastVoiceDetect = 0;
      recordAndSend();
    }
  } else {
    // Reset VAD hold timer if silence detected
    lastVoiceDetect = 0;
  }
}
