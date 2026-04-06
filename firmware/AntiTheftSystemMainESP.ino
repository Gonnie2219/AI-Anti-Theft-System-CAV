/*
 * ANTI-THEFT SYSTEM — Main ESP32
 * Central coordinator: reads sensors, buffers photo from CAM, forwards to LILYGO.
 * Board: ESP32 Dev Module
 *
 * Core 1 (main loop): RF remote, sensors, LED control — always responsive.
 * Core 0 (alarm task): Photo capture and UART forwarding run in background.
 *
 * UART0 (Serial)       = USB debug
 * UART2 (Serial2)      = ESP32-CAM (TX=17, RX=16)
 * UART1 (SerialLilyGO) = LILYGO    (TX=27, RX=26)
 */

#include <RCSwitch.h>

#define VIBRATION_PIN   13
#define REED_PIN        14
#define RF_PIN          15
#define CAM_TX          17
#define CAM_RX          16
#define LILYGO_TX       27
#define LILYGO_RX       26

// Status LEDs
#define LED_GREEN       32
#define LED_RED         33

RCSwitch mySwitch = RCSwitch();
#define CODE_ARM        616609
#define CODE_DISARM     616610

volatile bool armed = false;
bool lastVibration = LOW;
bool lastReed = LOW;
unsigned long lastAlarmTime = 0;
#define DEBOUNCE_MS 5000

// Image buffer (imgBuffer allocated once in setup; imgSize only written/read in alarm task on Core 0)
#define IMG_BUF_SIZE 51200
uint8_t* imgBuffer = NULL;
size_t imgSize = 0;

// Alarm task control
volatile bool alarmInProgress = false;

HardwareSerial SerialLilyGO(1);
SemaphoreHandle_t lilygoMutex;

// Forward declaration
void alarmTask(void* param);

void setup() {
  Serial.begin(115200);
  Serial.println("\n====================================");
  Serial.println("   Anti-Theft System");
  Serial.println("   Main ESP32");
  Serial.println("====================================\n");

  imgBuffer = (uint8_t*)malloc(IMG_BUF_SIZE);
  if (imgBuffer) {
    Serial.println("Image buffer allocated (50 KB)");
  } else {
    Serial.println("Image buffer allocation FAILED");
  }

  pinMode(VIBRATION_PIN, INPUT);
  pinMode(REED_PIN, INPUT);
  mySwitch.enableReceive(RF_PIN);

  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, HIGH);

  Serial2.setRxBufferSize(2048);
  Serial2.begin(115200, SERIAL_8N1, CAM_RX, CAM_TX);
  SerialLilyGO.begin(115200, SERIAL_8N1, LILYGO_RX, LILYGO_TX);

  lilygoMutex = xSemaphoreCreateMutex();

  Serial.println("UART2  ->  ESP32-CAM");
  Serial.println("UART1  ->  LILYGO");
  Serial.println("System ready - DISARMED\n");
}

// ── Main Loop (Core 1) — Always responsive ──────────────────
void loop() {
  // RF Remote — always checked, even during alarm
  if (mySwitch.available()) {
    unsigned long code = mySwitch.getReceivedValue();
    mySwitch.resetAvailable();
    if (code == CODE_ARM && !armed) {
      armed = true;
      digitalWrite(LED_GREEN, HIGH);
      digitalWrite(LED_RED, LOW);
      Serial.println("System armed");
      if (xSemaphoreTake(lilygoMutex, pdMS_TO_TICKS(100))) {
        if (!alarmInProgress) SerialLilyGO.println("STATUS:ARMED");
        xSemaphoreGive(lilygoMutex);
      }
    } else if (code == CODE_DISARM && armed) {
      armed = false;
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_RED, HIGH);
      Serial.println("System disarmed");
      if (xSemaphoreTake(lilygoMutex, pdMS_TO_TICKS(100))) {
        if (!alarmInProgress) SerialLilyGO.println("STATUS:DISARMED");
        xSemaphoreGive(lilygoMutex);
      }
    }
  }

  if (!armed) return;

  // Sensors — only trigger if no alarm already in progress
  if (!alarmInProgress) {
    bool vibration = digitalRead(VIBRATION_PIN);
    if (vibration == HIGH && lastVibration == LOW) startAlarm("Vibration");
    lastVibration = vibration;

    bool reed = digitalRead(REED_PIN);
    if (reed == HIGH && lastReed == LOW) startAlarm("Door Opened");
    lastReed = reed;
  }

  // Drain any LILYGO responses on main loop
  if (!alarmInProgress && xSemaphoreTake(lilygoMutex, pdMS_TO_TICKS(50))) {
    if (SerialLilyGO.available()) {
      String r = SerialLilyGO.readStringUntil('\n'); r.trim();
      if (r.length() > 0) Serial.println("LILYGO: " + r);
    }
    xSemaphoreGive(lilygoMutex);
  }
}

// ── Start Alarm (launches background task on Core 0) ────────
void startAlarm(String reason) {
  unsigned long now = millis();
  if (now - lastAlarmTime < DEBOUNCE_MS) return;
  lastAlarmTime = now;

  alarmInProgress = true;
  Serial.println("");
  Serial.println("*** ALARM TRIGGERED: " + reason + " ***");

  // Launch alarm task on Core 0 with 16KB stack
  char* reasonCopy = strdup(reason.c_str());
  xTaskCreatePinnedToCore(
    alarmTask,       // function
    "AlarmTask",     // name
    16384,           // stack size (16KB — heavy String usage)
    reasonCopy,      // parameter
    1,               // priority
    NULL,            // task handle
    0                // Core 0
  );
}

// ── Alarm Task (Core 0) — Runs in background ────────────────
void alarmTask(void* param) {
  String reason = (char*)param;
  free(param);

  Serial.println("  -> Notifying LILYGO");
  xSemaphoreTake(lilygoMutex, portMAX_DELAY);
  SerialLilyGO.println("ALERT:" + reason);
  xSemaphoreGive(lilygoMutex);

  Serial.println("  -> Requesting photo");
  Serial2.println("PHOTO");

  imgSize = 0;
  bool photoOK = receivePhotoFromCAM();

  if (photoOK && imgSize > 0) {
    Serial.println("  -> Forwarding photo to LILYGO (" + String(imgSize) + " bytes)");
    xSemaphoreTake(lilygoMutex, portMAX_DELAY);
    SerialLilyGO.println("IMG:" + String(imgSize));
    delay(50);

    size_t sent = 0;
    while (sent < imgSize) {
      size_t toSend = imgSize - sent;
      if (toSend > 512) toSend = 512;
      SerialLilyGO.write(imgBuffer + sent, toSend);
      SerialLilyGO.flush();
      sent += toSend;
      delay(5);
    }
    delay(50);
    SerialLilyGO.println("IMG_END");
    xSemaphoreGive(lilygoMutex);
    Serial.println("  -> Photo forwarded");
  } else {
    Serial.println("  -> No photo available");
    xSemaphoreTake(lilygoMutex, portMAX_DELAY);
    SerialLilyGO.println("NOIMG");
    xSemaphoreGive(lilygoMutex);
  }

  Serial.println("  -> Waiting for LILYGO");
  unsigned long timeout = millis() + 90000;
  while (millis() < timeout) {
    if (xSemaphoreTake(lilygoMutex, pdMS_TO_TICKS(50))) {
      if (SerialLilyGO.available()) {
        String r = SerialLilyGO.readStringUntil('\n'); r.trim();
        if (r.length() > 0) {
          Serial.println("LILYGO: " + r);
          xSemaphoreGive(lilygoMutex);
          if (r.startsWith("LILYGO_OK") || r.startsWith("LILYGO_ERROR")) break;
          continue;
        }
      }
      xSemaphoreGive(lilygoMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(10));  // Yield to other tasks
  }

  Serial.println("Alarm handled.\n");
  alarmInProgress = false;

  // Delete this task when done
  vTaskDelete(NULL);
}

// ── Receive Photo from CAM ──────────────────────────────────
bool receivePhotoFromCAM() {
  unsigned long timeout = millis() + 10000;
  bool gotHeader = false;
  size_t expectedSize = 0;

  while (millis() < timeout) {
    if (Serial2.available()) {
      if (!gotHeader) {
        String line = Serial2.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
          Serial.println("  CAM: " + line);

          if (line.startsWith("IMG:")) {
            expectedSize = line.substring(4).toInt();
            if (expectedSize > 0 && expectedSize <= IMG_BUF_SIZE && imgBuffer) {
              gotHeader = true;
              imgSize = 0;
              Serial.println("  Receiving " + String(expectedSize) + " bytes...");
            } else {
              Serial.println("  Invalid image size: " + String(expectedSize));
              return false;
            }
          }
        }
      } else {
        size_t avail = Serial2.available();
        size_t toRead = expectedSize - imgSize;
        if (avail < toRead) toRead = avail;
        if (toRead > 0) {
          Serial2.readBytes(imgBuffer + imgSize, toRead);
          imgSize += toRead;
        }

        if (imgSize >= expectedSize) {
          delay(100);
          while (Serial2.available()) {
            String end = Serial2.readStringUntil('\n');
            end.trim();
            if (end.length() > 0) Serial.println("  CAM: " + end);
          }
          Serial.println("  Image buffered (" + String(imgSize) + " bytes)");
          return true;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));  // Yield briefly
  }

  if (gotHeader) {
    Serial.println("  Photo timeout (" + String(imgSize) + "/" + String(expectedSize) + " bytes)");
  } else {
    Serial.println("  Photo timeout (no header)");
  }
  return false;
}
