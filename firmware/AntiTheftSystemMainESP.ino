/*
 * ANTI-THEFT SYSTEM — Main ESP32
 * Central coordinator: reads sensors, buffers photo from CAM, forwards to LILYGO.
 * Handles SMS commands (ARM, DISARM, STATUS, PHOTO, IMMOBILIZE, RESTORE) forwarded from LILYGO.
 * Board: ESP32 Dev Module
 *
 * Core 1 (main loop): RF remote, sensors, LED control — always responsive.
 * Core 0 (alarm task): Photo capture and UART forwarding run in background.
 *
 * UART0 (Serial)       = USB debug
 * UART2 (Serial2)      = ESP32-CAM (TX=17, RX=16)
 * UART1 (SerialLilyGO) = LILYGO    (TX=27, RX=26)
 *
 * SpeedTalk migration (May 2026):
 * - Removed the destructive SerialLilyGO drain inside the SMS_CMD: handler.
 *   Old code did `while (avail) read()` immediately after pulling SMS_CMD,
 *   which discarded any back-to-back SMS_CMD lines or pending status replies
 *   that happened to be queued. Now we just process the one line we received
 *   and let the loop pick up the rest naturally.
 */

#include <RCSwitch.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

#define VIBRATION_PIN   13
#define REED_PIN        14
#define RF_PIN          15
#define CAM_TX          17
#define CAM_RX          16
#define LILYGO_TX       27
#define LILYGO_RX       26
#define IMMOBILIZE_PIN  23

// Status LEDs
#define LED_GREEN       32
#define LED_RED         33

RCSwitch mySwitch = RCSwitch();
#define CODE_ARM        616609
#define CODE_DISARM     616610

volatile bool armed = false;
bool immobilized = false;
Preferences preferences;

bool lastVibration = LOW;
bool lastReed = LOW;
unsigned long lastAlarmTime = 0;
#define DEBOUNCE_MS 5000

// Image buffer (allocated once in setup; only written/read in alarm task on Core 0)
#define IMG_BUF_SIZE 51200
uint8_t* imgBuffer = NULL;
size_t imgSize = 0;

// Alarm task control
volatile bool alarmInProgress = false;

HardwareSerial SerialLilyGO(1);
SemaphoreHandle_t lilygoMutex;

// MPU-6050 motion detection
Adafruit_MPU6050 mpu;
bool mpuOK = false;
float gravityMag = 9.81;

enum MotionState { MOTION_UNKNOWN, MOTION_STATIONARY, MOTION_MOVING };
volatile MotionState motionState = MOTION_UNKNOWN;
volatile float lastAccelDelta = 0.0;  // m/s^2, low-pass filtered ||a||-gravityMag
volatile float lastGyroMag = 0.0;     // rad/s, low-pass filtered
volatile float lastGpsSpeedMph = -1.0; // -1 = unknown, fed from LILYGO

// Motion smoothing ring buffers
#define MOTION_SAMPLES 10
float accelRing[MOTION_SAMPLES];
float gyroRing[MOTION_SAMPLES];
int ringIndex = 0;
int ringSampled = 0;

// Motion hysteresis timing
unsigned long motionConditionStart = 0;
bool motionConditionAbove = false;

#define ACCEL_TH 0.5f    // m/s^2
#define GYRO_TH  0.2f    // rad/s

// Motion read / push timing
unsigned long lastMotionMs = 0;
unsigned long lastMotionPushMs = 0;

// Forward declarations
void alarmTask(void* param);
void handleSMSCommand(String cmd);
bool receivePhotoFromCAM();
void startAlarm(String reason);
void readMotion();

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

  // Immobilization relay — persisted via NVS so state survives reboot
  preferences.begin("antitheft", false);
  immobilized = preferences.getBool("immobilized", false);
  pinMode(IMMOBILIZE_PIN, OUTPUT);
  digitalWrite(IMMOBILIZE_PIN, immobilized ? LOW : HIGH);
  Serial.println(immobilized ? "Ignition: IMMOBILIZED (relay off)" : "Ignition: OK (relay on)");

  // Armed state — persisted via NVS so state survives reboot
  armed = preferences.getBool("armed", false);
  digitalWrite(LED_GREEN, armed ? HIGH : LOW);
  digitalWrite(LED_RED, armed ? LOW : HIGH);
  Serial.println(armed ? "System: ARMED (restored)" : "System: DISARMED");

  Serial2.setRxBufferSize(2048);
  Serial2.begin(115200, SERIAL_8N1, CAM_RX, CAM_TX);
  SerialLilyGO.setRxBufferSize(4096);
  SerialLilyGO.begin(115200, SERIAL_8N1, LILYGO_RX, LILYGO_TX);

  lilygoMutex = xSemaphoreCreateMutex();

  Serial.println("UART2  ->  ESP32-CAM");
  Serial.println("UART1  ->  LILYGO");

  // MPU-6050 initialization
  Wire.begin(21, 22);
  if (!mpu.begin()) {
    Serial.println("MPU6050 FAILED");
    mpuOK = false;
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    mpuOK = true;
    Serial.println("MPU6050 OK");
  }

  if (mpuOK) {
    Serial.println("Calibrating gravity (keep vehicle stationary)...");
    float magSum = 0.0;
    int calSamples = 100;
    for (int i = 0; i < calSamples; i++) {
      sensors_event_t a, g, temp;
      mpu.getEvent(&a, &g, &temp);
      float mag = sqrt(a.acceleration.x * a.acceleration.x +
                       a.acceleration.y * a.acceleration.y +
                       a.acceleration.z * a.acceleration.z);
      magSum += mag;
      delay(20);  // ~50 Hz -> ~2s total
    }
    gravityMag = magSum / calSamples;
    Serial.println("Calibrated gravityMag = " + String(gravityMag, 3) + " m/s^2");
  }

  Serial.println(mpuOK ? "Motion logic: ACTIVE (MPU-6050 online)"
                        : "Motion logic: BLIND (no MPU-6050)");

  // Wait for CAM_READY and LILYGO_READY (up to 60s — LILYGO modem boot is slow).
  // Prevents a first-alarm race where the user triggers a sensor before
  // peripherals finish booting.
  Serial.println("Waiting for peripherals...");
  bool camReady = false, lilygoReady = false;
  unsigned long bootDeadline = millis() + 60000;
  while (millis() < bootDeadline && !(camReady && lilygoReady)) {
    if (Serial2.available()) {
      String line = Serial2.readStringUntil('\n'); line.trim();
      if (line.length() > 0) {
        Serial.println("  CAM: " + line);
        if (line == "CAM_READY") camReady = true;
      }
    }
    if (SerialLilyGO.available()) {
      String line = SerialLilyGO.readStringUntil('\n'); line.trim();
      if (line.length() > 0) {
        Serial.println("  LILYGO: " + line);
        if (line == "LILYGO_READY") lilygoReady = true;
      }
    }
    delay(10);
  }
  if (!camReady)    Serial.println("  CAM did not report ready");
  if (!lilygoReady) Serial.println("  LILYGO did not report ready");

  Serial.println(armed ? "System ready - ARMED\n" : "System ready - DISARMED\n");
}

// ── Motion Reading (called from loop every 50 ms) ───────────
void readMotion() {
  if (!mpuOK) return;

  unsigned long now = millis();
  if (now - lastMotionMs < 50) return;
  lastMotionMs = now;

  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  float accelMag = sqrt(a.acceleration.x * a.acceleration.x +
                        a.acceleration.y * a.acceleration.y +
                        a.acceleration.z * a.acceleration.z);
  float accelDelta = fabs(accelMag - gravityMag);
  float gyroMag = sqrt(g.gyro.x * g.gyro.x +
                       g.gyro.y * g.gyro.y +
                       g.gyro.z * g.gyro.z);

  // Ring buffer smoothing
  accelRing[ringIndex] = accelDelta;
  gyroRing[ringIndex] = gyroMag;
  ringIndex = (ringIndex + 1) % MOTION_SAMPLES;
  if (ringSampled < MOTION_SAMPLES) ringSampled++;

  float accelSum = 0.0, gyroSum = 0.0;
  for (int i = 0; i < ringSampled; i++) {
    accelSum += accelRing[i];
    gyroSum += gyroRing[i];
  }
  lastAccelDelta = accelSum / ringSampled;
  lastGyroMag = gyroSum / ringSampled;

  // Hysteresis state machine
  bool above = (lastAccelDelta >= ACCEL_TH) || (lastGyroMag >= GYRO_TH);

  if (above != motionConditionAbove) {
    motionConditionAbove = above;
    motionConditionStart = now;
  }

  unsigned long elapsed = now - motionConditionStart;

  if (above && elapsed >= 500) {
    motionState = MOTION_MOVING;
  } else if (!above && elapsed >= 2000) {
    motionState = MOTION_STATIONARY;
  }
  // Otherwise hold current state
}

// ── Main Loop (Core 1) — Always responsive ──────────────────
void loop() {
  // RF Remote — always checked, even during alarm
  if (mySwitch.available()) {
    unsigned long code = mySwitch.getReceivedValue();
    mySwitch.resetAvailable();
    if (code == CODE_ARM && !armed) {
      armed = true;
      preferences.putBool("armed", true);
      digitalWrite(LED_GREEN, HIGH);
      digitalWrite(LED_RED, LOW);
      Serial.println("System armed");
      if (xSemaphoreTake(lilygoMutex, pdMS_TO_TICKS(100))) {
        if (!alarmInProgress) SerialLilyGO.println("STATUS:ARMED");
        xSemaphoreGive(lilygoMutex);
      }
    } else if (code == CODE_DISARM && armed) {
      armed = false;
      preferences.putBool("armed", false);
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_RED, HIGH);
      Serial.println("System disarmed");
      if (xSemaphoreTake(lilygoMutex, pdMS_TO_TICKS(100))) {
        if (!alarmInProgress) SerialLilyGO.println("STATUS:DISARMED");
        xSemaphoreGive(lilygoMutex);
      }
    }
  }

  // MPU-6050 motion sampling
  readMotion();

  // Drain LILYGO responses + handle remote arm/disarm/SMS commands
  // (must run even when disarmed)
  if (!alarmInProgress && xSemaphoreTake(lilygoMutex, pdMS_TO_TICKS(50))) {
    if (SerialLilyGO.available()) {
      String r = SerialLilyGO.readStringUntil('\n'); r.trim();
      if (r.length() > 0) {
        Serial.println("LILYGO: " + r);
        if (r == "REQUEST_PHOTO") {
          startAlarm("Photo Requested");
        } else if (r.startsWith("SMS_CMD:")) {
          // Process the SMS command. We deliberately do NOT drain SerialLilyGO
          // here — that would discard back-to-back SMS_CMD lines or pending
          // status replies. The next loop iteration will pick those up.
          String smsCmd = r.substring(8);
          handleSMSCommand(smsCmd);
        } else if (r.startsWith("GPS_SPEED:")) {
          lastGpsSpeedMph = r.substring(10).toFloat();
        }
      }
    }
    xSemaphoreGive(lilygoMutex);
  }

  // Periodic motion push to LILYGO
  if (!alarmInProgress) {
    unsigned long motionInterval = armed ? 5000 : 30000;
    unsigned long now = millis();
    if (now - lastMotionPushMs >= motionInterval) {
      lastMotionPushMs = now;
      if (xSemaphoreTake(lilygoMutex, pdMS_TO_TICKS(100))) {
        String motStr = (motionState == MOTION_STATIONARY) ? "STATIONARY" :
                        (motionState == MOTION_MOVING)     ? "MOVING" : "UNKNOWN";
        SerialLilyGO.println("MOTION:" + motStr + ",A:" + String(lastAccelDelta, 2) +
                             ",G:" + String(lastGyroMag, 2));
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
  if (!reasonCopy) {
    Serial.println("AlarmTask: strdup failed (OOM)");
    alarmInProgress = false;
    return;
  }
  BaseType_t rc = xTaskCreatePinnedToCore(
    alarmTask,       // function
    "AlarmTask",     // name
    16384,           // stack size (16KB — heavy String usage)
    reasonCopy,      // parameter
    1,               // priority
    NULL,            // task handle
    0                // Core 0
  );
  if (rc != pdPASS) {
    Serial.println("AlarmTask creation failed!");
    free(reasonCopy);
    alarmInProgress = false;
  }
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
          // During alarm, only DISARM and RESTORE make sense to act on.
          // DISARM sets armed=false so sensors won't re-trigger after this
          // alarm completes.  Photo-forward is already finished at this
          // point — we're just waiting for LILYGO_OK.  Other commands
          // (PHOTO, GPS, STATUS) are logged and dropped.
          if (r.startsWith("SMS_CMD:")) {
            String smsCmd = r.substring(8);
            if (smsCmd == "DISARM" || smsCmd == "RESTORE") {
              handleSMSCommand(smsCmd);
            } else {
              Serial.println("[CMD] dropped (alarm active): " + smsCmd);
            }
          } else if (r.startsWith("GPS_SPEED:")) {
            lastGpsSpeedMph = r.substring(10).toFloat();
          }
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
  // Relaxed from 10s -> 15s. UART image transfer at 115200 baud takes
  // ~5-6 seconds for a 50 KB JPEG; the extra 5s gives margin for CAM
  // burst capture (3 frames + SD writes) before transfer starts.
  unsigned long timeout = millis() + 15000;
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

// ── SMS Command Handler ──────────────────────────────────────
void handleSMSCommand(String cmd) {
  Serial.println("SMS command: " + cmd);

  if (cmd == "ARM") {
    if (!armed) {
      armed = true;
      preferences.putBool("armed", true);
      digitalWrite(LED_GREEN, HIGH);
      digitalWrite(LED_RED, LOW);
      Serial.println("Armed via SMS");
      SerialLilyGO.println("SMS_REPLY:System armed");
      SerialLilyGO.println("STATUS:ARMED");
    } else {
      SerialLilyGO.println("SMS_REPLY:Already armed");
    }
  } else if (cmd == "DISARM") {
    if (armed) {
      armed = false;
      preferences.putBool("armed", false);
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_RED, HIGH);
      Serial.println("Disarmed via SMS");
      SerialLilyGO.println("SMS_REPLY:System disarmed");
      SerialLilyGO.println("STATUS:DISARMED");
    } else {
      SerialLilyGO.println("SMS_REPLY:Already disarmed");
    }
  } else if (cmd == "STATUS") {
    String status = armed ? "ARMED" : "DISARMED";
    String reply = "Status: " + status;
    reply += "\nIgnition: " + String(immobilized ? "IMMOBILIZED" : "OK");
    String motStr = (motionState == MOTION_STATIONARY) ? "STATIONARY" :
                    (motionState == MOTION_MOVING)     ? "MOVING" : "UNKNOWN";
    reply += "\nMotion: " + motStr + " A:" + String(lastAccelDelta, 2) + " G:" + String(lastGyroMag, 2);
    if (lastGpsSpeedMph >= 0.0) {
      reply += "\nGPS Speed: " + String(lastGpsSpeedMph, 1) + " mph";
    }
    if (lastAlarmTime > 0) {
      unsigned long ago = (millis() - lastAlarmTime) / 1000;
      if (ago < 60)        reply += "\nLast alarm: " + String(ago) + "s ago";
      else if (ago < 3600) reply += "\nLast alarm: " + String(ago / 60) + "m ago";
      else                 reply += "\nLast alarm: " + String(ago / 3600) + "h ago";
    } else {
      reply += "\nNo alarms since boot";
    }
    SerialLilyGO.println("SMS_REPLY:" + reply);
  } else if (cmd == "PHOTO") {
    SerialLilyGO.println("SMS_REPLY:Capturing photo...");
    startAlarm("Photo Requested");
  } else if (cmd == "IMMOBILIZE") {
    if (!immobilized) {
      // Safety interlock: refuse if any available sensor reports motion
      bool gpsKnown   = (lastGpsSpeedMph >= 0.0);
      bool gpsStill   = gpsKnown && (lastGpsSpeedMph < 2.0);
      bool gpsRefuses = gpsKnown && !gpsStill;
      bool mpuRefuses = mpuOK && (motionState == MOTION_MOVING);
      bool isSensorBlind = (!mpuOK && !gpsKnown);

      if (gpsRefuses || mpuRefuses) {
        SerialLilyGO.println("IMMOBILIZE_REJECTED:MOVING,A:" + String(lastAccelDelta, 2) +
                             ",G:" + String(lastGyroMag, 2) +
                             ",S:" + String(lastGpsSpeedMph, 1));
        Serial.println("IMMOBILIZE rejected — vehicle moving");
        return;
      }
      if (isSensorBlind) {
        Serial.println("WARN: immobilize granted with sensors blind (no GPS, no MPU)");
      }

      immobilized = true;
      digitalWrite(IMMOBILIZE_PIN, LOW);
      preferences.putBool("immobilized", true);
      Serial.println("Ignition immobilized via command");
      SerialLilyGO.println("SMS_REPLY:Ignition immobilized");
      SerialLilyGO.println("STATUS:IMMOBILIZED");
    } else {
      SerialLilyGO.println("SMS_REPLY:Already immobilized");
    }
  } else if (cmd == "RESTORE") {
    if (immobilized) {
      immobilized = false;
      digitalWrite(IMMOBILIZE_PIN, HIGH);
      preferences.putBool("immobilized", false);
      Serial.println("Ignition restored via command");
      SerialLilyGO.println("SMS_REPLY:Ignition restored");
      SerialLilyGO.println("STATUS:IGNITION_OK");
    } else {
      SerialLilyGO.println("SMS_REPLY:Already restored");
    }
  }
}
