/*
 * ARIA — ESP32-CAM (EYES)
 * Responsibilities:
 *   - Host a simple HTTP server
 *   - Capture JPEG frame on demand via /capture endpoint
 *   - Send frame back to Flask server for vision analysis
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>

// ================= WIFI =================
const char* ssid     = "ESPTEST";
const char* password = "12345678";

// ================= CAMERA MODEL =================
// Using AI Thinker ESP32-CAM board
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ================= WEB SERVER =================
WebServer server(80);

// ================= CAPTURE HANDLER =================
void handleCapture() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "Camera capture failed");
    return;
  }

  server.sendHeader("Content-Type", "image/jpeg");
  server.sendHeader("Content-Disposition", "inline; filename=capture.jpg");
  server.send_P(200, "image/jpeg", (const char*)fb->buf, fb->len);

  esp_camera_fb_return(fb);
  Serial.println("📷 Frame captured and sent");
}

void handleStatus() {
  server.send(200, "application/json", "{\"status\":\"ok\",\"camera\":\"ready\"}");
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  Serial.println("ARIA Camera booting...");

  // Camera config
  camera_config_t config;
  config.ledc_channel  = LEDC_CHANNEL_0;
  config.ledc_timer    = LEDC_TIMER_0;
  config.pin_d0        = Y2_GPIO_NUM;
  config.pin_d1        = Y3_GPIO_NUM;
  config.pin_d2        = Y4_GPIO_NUM;
  config.pin_d3        = Y5_GPIO_NUM;
  config.pin_d4        = Y6_GPIO_NUM;
  config.pin_d5        = Y7_GPIO_NUM;
  config.pin_d6        = Y8_GPIO_NUM;
  config.pin_d7        = Y9_GPIO_NUM;
  config.pin_xclk      = XCLK_GPIO_NUM;
  config.pin_pclk      = PCLK_GPIO_NUM;
  config.pin_vsync     = VSYNC_GPIO_NUM;
  config.pin_href      = HREF_GPIO_NUM;
  config.pin_sccb_sda  = SIOD_GPIO_NUM;
  config.pin_sccb_scl  = SIOC_GPIO_NUM;
  config.pin_pwdn      = PWDN_GPIO_NUM;
  config.pin_reset     = RESET_GPIO_NUM;
  config.xclk_freq_hz  = 20000000;
  config.frame_size    = FRAMESIZE_QVGA;   // 320x240 — good balance of quality vs speed
  config.pixel_format  = PIXFORMAT_JPEG;
  config.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location   = CAMERA_FB_IN_DRAM;
  config.jpeg_quality  = 12;
  config.fb_count      = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("❌ Camera init failed: 0x%x\n", err);
    return;
  }
  Serial.println("✅ Camera initialized");

  // WiFi connect
  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  Serial.print("Camera IP: ");
  Serial.println(WiFi.localIP());

  // Register routes
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/status",  HTTP_GET, handleStatus);
  server.begin();

  Serial.println("📷 Camera server ready!");
  Serial.println("Access: http://" + WiFi.localIP().toString() + "/capture");
}

// ================= LOOP =================
void loop() {
  server.handleClient();
}
