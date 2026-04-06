/*
 * ANTI-THEFT SYSTEM — ESP32-CAM
 * Captures photo on PHOTO command, saves to SD card, streams over UART.
 * Board: AI Thinker ESP32-CAM
 *
 * Protocol on PHOTO command:
 *   1. "CAM_OK: Photo saved /photo_N.jpg (X KB)\n" (x3 burst)
 *   2. "IMG:<byte-count>\n"
 *   3. Raw JPEG bytes in 512-byte chunks
 *   4. "IMG_END\n"
 */

#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"

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
#define FLASH_LED_PIN      4

int photoCount = 0;

void setup() {
  Serial.begin(115200);
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_VGA;
  config.jpeg_quality = 10;
  config.fb_count     = 1;

  if (psramFound()) {
    config.fb_count     = 2;
  } else {
    config.frame_size = FRAMESIZE_CIF;
  }

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("CAM_ERROR: Camera init failed");
    return;
  }

  if (!SD_MMC.begin()) {
    Serial.println("CAM_ERROR: SD init failed");
    return;
  }

  Serial.println("CAM_READY");
}

void loop() {
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    if (command == "PHOTO") {
      takePhoto();
    }
  }
}

void takePhoto() {
  digitalWrite(FLASH_LED_PIN, HIGH);
  delay(100);

  // Discard first frame (may be stale or poorly exposed)
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);

  // Take 3 photos in burst, save all to SD, send last one
  camera_fb_t *sendFb = NULL;
  for (int shot = 0; shot < 3; shot++) {
    if (sendFb) esp_camera_fb_return(sendFb);
    sendFb = NULL;

    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("CAM_ERROR: Capture " + String(shot + 1) + " failed");
      continue;
    }

    photoCount++;
    String filename = "/photo_" + String(photoCount) + ".jpg";
    File file = SD_MMC.open(filename, FILE_WRITE);
    if (file) {
      size_t written = file.write(fb->buf, fb->len);
      file.close();
      if (written != fb->len) Serial.println("CAM_ERROR: SD write incomplete");
    }

    Serial.println("CAM_OK: Photo saved " + filename + " (" + String(fb->len / 1024) + " KB)");
    sendFb = fb;

    if (shot < 2) delay(300);
  }

  digitalWrite(FLASH_LED_PIN, LOW);

  if (!sendFb) {
    Serial.println("CAM_ERROR: All captures failed");
    return;
  }

  // Send last photo over UART
  Serial.println("IMG:" + String(sendFb->len));
  delay(50);

  size_t sent = 0;
  while (sent < sendFb->len) {
    size_t toSend = sendFb->len - sent;
    if (toSend > 512) toSend = 512;
    Serial.write(sendFb->buf + sent, toSend);
    Serial.flush();
    sent += toSend;
    delay(5);
  }

  delay(50);
  Serial.println("IMG_END");

  esp_camera_fb_return(sendFb);
}
