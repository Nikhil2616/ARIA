/*
 * ARIA — ESP32_A (EAR)
 * Responsibilities:
 *   - Capture voice via INMP441 microphone
 *   - Detect wake word in transcribed text
 *   - Stream audio to Flask server
 *   - Read sensor data from Arduino via Serial
 *   - Post sensor data to Flask server
 *   - Receive motor commands from Flask server
 *   - Forward motor commands to Arduino via Serial
 *   - Show status on OLED display
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "driver/i2s.h"

// ================= WIFI =================
const char* ssid     = "ESPTEST";
const char* password = "12345678";

// ================= SERVER =================
const char* serverHost = "10.84.51.142"; // ⚠️ Your PC's IP address
const int   serverPort    = 5000;
const char* sensorURL  = "http://10.84.51.142:5000/update-sensor";

// ================= WAKE WORD =================
// ⚠️ SET YOUR WAKE WORD HERE WHEN DECIDED
const char* WAKE_WORD = "ARIA";

// ================= PINS =================
#define BUTTON_PIN  4
#define LED_PIN     2

// INMP441 I2S Microphone
#define I2S_WS      25
#define I2S_SD      34
#define I2S_SCK     26

// ================= AUDIO =================
#define SAMPLE_RATE     16000
#define RECORD_SECONDS  5
#define BUFFER_SIZE     1024

// ================= OLED =================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire);

// ================= STATE =================
bool recording       = false;
bool lastButtonState = HIGH;
int  frontDist       = 999;
int  backDist        = 999;
unsigned long lastSensorPost = 0;
#define SENSOR_POST_INTERVAL 1000  // post sensor data every 1 second

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

// ================= RECORD + SEND =================
void recordAndSend() {
  recording = true;
  digitalWrite(LED_PIN, HIGH);

  WiFiClient client;
  if (!client.connect(serverHost, serverPort)) {
    showOLED("SERVER ERROR", "Check IP/WiFi");
    recording = false;
    digitalWrite(LED_PIN, LOW);
    return;
  }

  int totalBytes = SAMPLE_RATE * 2 * RECORD_SECONDS;

  // Send HTTP POST with wake word in header
  client.println("POST /process-audio HTTP/1.1");
  client.printf("Host: %s:%d\r\n", serverHost, serverPort);
  client.println("Content-Type: application/octet-stream");
  client.printf("X-Wake-Word: %s\r\n", WAKE_WORD);
  client.printf("Content-Length: %d\r\n\r\n", totalBytes);

  char buffer[BUFFER_SIZE];
  size_t bytesRead;
  int sent = 0;

  // Flush junk audio from buffer
  i2s_read(I2S_NUM_0, buffer, BUFFER_SIZE, &bytesRead, portMAX_DELAY);

  showOLED("LISTENING...", "Speak now!");

  // Stream audio in chunks
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
}

// ================= POST SENSOR DATA =================
void postSensorData() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(sensorURL);
  http.addHeader("Content-Type", "application/json");

  String body = "{\"front\":" + String(frontDist) + ",\"back\":" + String(backDist) + "}";
  int code = http.POST(body);

  if (code != 200) {
    Serial.println("⚠️ Sensor post failed: " + String(code));
  }
  http.end();
}

// ================= READ SERIAL FROM ARDUINO =================
void readArduinoSerial() {
  while (Serial2.available()) {
    String line = Serial2.readStringUntil('\n');
    line.trim();

    // Format: "DIST:F:34:B:80"
    if (line.startsWith("DIST:")) {
      int fIdx = line.indexOf("F:");
      int bIdx = line.indexOf("B:");
      if (fIdx != -1 && bIdx != -1) {
        frontDist = line.substring(fIdx + 2, bIdx - 1).toInt();
        backDist  = line.substring(bIdx + 2).toInt();
        Serial.printf("📡 Front: %d cm | Back: %d cm\n", frontDist, backDist);
      }
    }
  }
}

// ================= HTTP SERVER FOR MOTOR COMMANDS =================
// ESP32 acts as a simple HTTP server to receive motor commands from Flask
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

  // Read body
  String body = "";
  while (client.available()) {
    body += (char)client.read();
  }

  // Parse direction and speed from JSON body
  // e.g. {"direction":"forward","speed":"normal"}
  String direction = "stop";
  String speed     = "normal";

  if (body.indexOf("\"forward\"") != -1) direction = "forward";
  else if (body.indexOf("\"backward\"") != -1) direction = "backward";
  else if (body.indexOf("\"left\"") != -1) direction = "left";
  else if (body.indexOf("\"right\"") != -1) direction = "right";
  else if (body.indexOf("\"stop\"") != -1) direction = "stop";

  if (body.indexOf("\"slow\"") != -1) speed = "slow";
  else if (body.indexOf("\"fast\"") != -1) speed = "fast";

  // Forward command to Arduino via Serial2
  String cmd = direction + ":" + speed + "\n";
  Serial2.print(cmd);
  Serial.println("🤖 Motor command → Arduino: " + cmd);

  // Update OLED
  showOLED("MOVING", direction, speed);

  // Send response
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Length: 2");
  client.println();
  client.println("OK");
  client.stop();
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  // Serial2 for Arduino communication (RX=16, TX=17)
  Serial2.begin(9600, SERIAL_8N1, 16, 17);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  // OLED init
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED failed");
    for (;;);
  }
  showOLED("ARIA Booting...");

  // WiFi connect
  WiFi.begin(ssid, password);
  showOLED("Connecting WiFi...");
  while (WiFi.status() != WL_CONNECTED) delay(300);

  showOLED("WiFi OK", WiFi.localIP().toString());
  Serial.println("WiFi IP: " + WiFi.localIP().toString());

  // I2S mic
  i2s_install();

  // Start motor command server
  motorServer.begin();

  showOLED("ARIA READY", "Say " + String(WAKE_WORD), "or press button");
  delay(1000);
}

// ================= LOOP =================
void loop() {
  // Read sensor data from Arduino
  readArduinoSerial();

  // Post sensor data to server every 1 second
  if (millis() - lastSensorPost > SENSOR_POST_INTERVAL) {
    postSensorData();
    lastSensorPost = millis();
  }

  // Handle incoming motor commands from Flask server
  handleMotorServer();

  // Button press → record and send
  if (digitalRead(BUTTON_PIN) == LOW && !recording) {
    delay(50); // debounce
    if (digitalRead(BUTTON_PIN) == LOW) {
      recordAndSend();
      while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    }
  }
}
