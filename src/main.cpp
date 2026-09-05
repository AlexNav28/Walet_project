#include <Arduino.h>
#include <Wire.h>
#include <ESP32Servo.h>
#include "SparkFun_FPC2534.h"
#include "secrets.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ============================================================================
// CONFIGURATION & UUID DEFINITIONS
// ============================================================================
#define SERVICE_UUID        "4FAFC201-1FB5-459E-8FCC-C5C9C331914B"
#define STATUS_CHAR_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define COMMAND_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a9"

#define BLE_FLAG_CLEAR        0x00
#define BLE_FLAG_WRONG_FINGER 0x01
#define BLE_FLAG_IMU_MOVING   0x02
#define BLE_FLAG_ENROLLING    0x04

// Custom BLE Commands
#define BLE_CMD_ENROLL_START    0x01
#define BLE_CMD_DELETE_ALL      0x02
#define BLE_CMD_LIST_TEMPLATES  0x03
#define BLE_CMD_SERVO_OPENING  0x04

// Hardware Pinout
#define IMU_SDA         36
#define IMU_SCL         37
#define SERVO_PIN       1     // Routed to buzzer pad rework
#define IRQ_PIN         16
#define UART_TX_PIN     17
#define UART_RX_PIN     18

#define IMU_I2C_ADDR    0x6A
#define REG_CTRL1_XL    0x10
#define REG_CTRL2_G     0x11
#define REG_OUTX_L_G    0x22

const int LOCKED_ANGLE = 180;
const int UNLOCKED_ANGLE = 120;
#define SERVO_TRAVEL_MS   800
#define UNLOCK_TIMEOUT_MS 5000 // Automatically relocks after 5 seconds

// ============================================================================
// FREERTOS OBJECTS & STATE MACHINE
// ============================================================================
EventGroupHandle_t systemEvents;
#define FLAG_THEFT_DETECTED   (1 << 0)
#define FLAG_FINGER_TOUCHED   (1 << 1)
#define FLAG_AUTH_PASSED      (1 << 2)
#define FLAG_AUTH_FAILED      (1 << 3)

enum SystemState {
  ST_LOCKED,
  ST_AUTH_VERIFYING,
  ST_UNLOCKED,
  ST_TAMPER_ALARM
};

volatile SystemState currentState = ST_LOCKED;

QueueHandle_t bleCommandQueue;
QueueHandle_t alertMessageQueue;

BLECharacteristic* pStatusChar;
BLECharacteristic* pCommandChar;

Servo walletServo;
SfeFPC2534UART mySensor;
static sfDevFPC2534Callbacks_t cmd_cb = {0};

uint16_t numberOfTemplates = 0;
bool isInitialized = false;

// Task declarations
void TaskSystemManager(void *pvParameters);
void TaskIMUTelemetry(void *pvParameters);
void TaskBiometricAuth(void *pvParameters);
void TaskBackgroundAlerts(void *pvParameters);

void writeIMURegister(byte reg, byte value);
void software_reset_sensor(void);
void arm_identify_mode(void);

// ============================================================================
// BLE NOTIFICATIONS & CALLBACKS
// ============================================================================
void bleSetFlag(uint8_t flag) {
  if (pStatusChar != nullptr) {
    uint8_t val = flag;
    pStatusChar->setValue(&val, 1);
    pStatusChar->notify();
  }
}

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    std::string val = pChar->getValue();
    if (!val.empty()) {
      uint8_t cmd = (uint8_t)val[0];
      Serial.printf("[BLE CMD] Received: 0x%02X\n", cmd);
      xQueueSend(bleCommandQueue, &cmd, 0);
    }
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    Serial.println("[BLE] Phone connected.");
  }

  void onDisconnect(BLEServer* pServer) override {
    Serial.println("[BLE] Phone disconnected. Restarting advertising...");
    BLEDevice::getAdvertising()->start();

    const char* msg = " Wallet may be out of range — phone disconnected! ";
    xQueueSend(alertMessageQueue, &msg, 0);
  }
};

void initBLE() {
  BLEDevice::init("SmartWallet");
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  pStatusChar = pService->createCharacteristic(
    STATUS_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pStatusChar->addDescriptor(new BLE2902());

  pCommandChar = pService->createCharacteristic(
    COMMAND_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pCommandChar->setCallbacks(new CommandCallbacks());

  pService->start();
  BLEDevice::getAdvertising()->addServiceUUID(SERVICE_UUID);
  BLEDevice::getAdvertising()->start();
  Serial.println("[BLE] Advertising started.");
}

// ============================================================================
// BACKGROUND NETWORK ALERT TASK
// ============================================================================
void TaskBackgroundAlerts(void *pvParameters) {
  const char* msgPtr;
  for (;;) {
    if (xQueueReceive(alertMessageQueue, &msgPtr, portMAX_DELAY) == pdTRUE) {
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[NET] Connecting to Wi-Fi hotspot for alert delivery...");
        WiFi.disconnect(true);
        vTaskDelay(pdMS_TO_TICKS(100));
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASS);

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
          vTaskDelay(pdMS_TO_TICKS(500));
          attempts++;
        }
      }

      if (WiFi.status() == WL_CONNECTED) {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient https;
        if (https.begin(client, String(DISCORD_WEBHOOK))) {
          https.addHeader("Content-Type", "application/json");
          String payload = "{\"embeds\":[{\"title\":\"🚨 Smart Wallet Alert!\",\"description\":\"" 
                          + String(msgPtr) + "\",\"color\":16711680}]}";
          int httpCode = https.POST(payload);
          Serial.printf("[NET] Discord delivery response: %d\n", httpCode);
          https.end();
        }
      } else {
        Serial.println("[NET] Unable to deliver alert: Wi-Fi unreachable.");
      }
    }
  }
}

// ============================================================================
// SYSTEM SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  
  uint32_t serialTimeout = millis();
  while (!Serial && (millis() - serialTimeout < 4000)) {
    delay(10);
  }

  Serial.println("\n==================================================");
  Serial.println("[BOOT] Smart Wallet Firmware Starting...");
  Serial.println("==================================================");

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  
  pinMode(SERVO_PIN, OUTPUT);
  pinMode(IRQ_PIN, INPUT);

  walletServo.setPeriodHertz(50);
  walletServo.attach(SERVO_PIN, 500, 2400);
  walletServo.write(LOCKED_ANGLE);
  delay(SERVO_TRAVEL_MS);

  systemEvents = xEventGroupCreate();
  bleCommandQueue = xQueueCreate(5, sizeof(uint8_t));
  alertMessageQueue = xQueueCreate(4, sizeof(char*));

  initBLE();

  xTaskCreatePinnedToCore(TaskSystemManager,    "SysManager", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskBiometricAuth,    "BioAuth",    8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskIMUTelemetry,     "IMU_Track",  4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(TaskBackgroundAlerts, "NetAlert",   6144, NULL, 1, NULL, 0);

  Serial.println("[BOOT] Setup complete. Scheduler operational.\n");
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}

// ============================================================================
// FPC2534 CALLBACK INTERFACES
// ============================================================================
static void on_error(uint16_t error) {
  Serial.printf("[CALLBACK ERROR] FPC Sensor Error: 0x%04X\n", error);
}

static void on_is_ready_change(bool isReady) {
  Serial.printf("[CALLBACK READY] FPC Status: %s\n", isReady ? "READY" : "NOT_READY");
}

static void on_identify(bool is_match, uint16_t id) {
  if (is_match) {
    Serial.printf("[CALLBACK AUTH] Match verified! Template ID: %d\n", id);
    xEventGroupSetBits(systemEvents, FLAG_AUTH_PASSED);
    bleSetFlag(BLE_FLAG_CLEAR);
  } else {
    Serial.println("[CALLBACK AUTH] No match found. Access Denied.");
    xEventGroupSetBits(systemEvents, FLAG_AUTH_FAILED);
    bleSetFlag(BLE_FLAG_WRONG_FINGER);
    arm_identify_mode();
  }
}

static void on_enroll(uint8_t feedback, uint8_t samples_remaining) {
  Serial.printf("[ENROLL] Step feedback: %d | Samples remaining: %d\n", feedback, samples_remaining);
  if (samples_remaining == 0) {
    Serial.println("[ENROLL] Enrollment finished successfully!");
    bleSetFlag(BLE_FLAG_CLEAR);
    arm_identify_mode();
  }
}

static void on_list_templates(uint16_t num_templates, uint16_t *template_ids) {
  numberOfTemplates = num_templates;
  isInitialized = true;
  Serial.printf("[CALLBACK SYNC] Database synchronized. Templates Enrolled: %d\n", numberOfTemplates);
  if (template_ids != nullptr && num_templates > 0) {
    for (uint16_t i = 0; i < num_templates; i++) {
      Serial.printf("  -> Slot [%u]: ID %u\n", i, template_ids[i]);
    }
  }
}

static void on_status(uint16_t event, uint16_t state) {
  if (event == EVENT_FINGER_DETECT || (state & STATE_FINGER_DOWN)) {
    Serial.println("[FPC] Finger present on sensor array.");
    if (currentState == ST_LOCKED) {
      xEventGroupSetBits(systemEvents, FLAG_FINGER_TOUCHED);
    }
  }
}

void software_reset_sensor(void) {
  Serial.println("[FPC] Flushing UART and issuing soft reset...");
  while (Serial1.available()) { Serial1.read(); }
  mySensor.sendReset();
  vTaskDelay(pdMS_TO_TICKS(300));
}

void arm_identify_mode(void) {
  while (Serial1.available()) { Serial1.read(); }

  fpc_id_type_t id = {0};
  id.type = ID_TYPE_ALL;
  id.id = 0;

  fpc_result_t rc = mySensor.requestIdentify(id, 1);
  if (rc == FPC_RESULT_OK) {
    Serial.println("[BIOMETRIC] Scanner armed and waiting for finger.");
  } else {
    Serial.printf("[BIOMETRIC] Arm rejected (code %d)\n", rc);
  }
}

void writeIMURegister(byte reg, byte value) {
  Wire.beginTransmission(IMU_I2C_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

// ============================================================================
// TASK: SYSTEM STATE MANAGER (CORE 1)
// ============================================================================
void TaskSystemManager(void *pvParameters) {
  for (;;) {
    switch (currentState) {
      case ST_LOCKED: {                                                                                 // LOCKED
        EventBits_t bits = xEventGroupWaitBits(systemEvents, 
                                               FLAG_FINGER_TOUCHED | FLAG_THEFT_DETECTED | FLAG_AUTH_PASSED, 
                                               pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & FLAG_THEFT_DETECTED) {
          currentState = ST_TAMPER_ALARM;
          Serial.println("[STATE] -> ST_TAMPER_ALARM");
        } else if (bits & FLAG_AUTH_PASSED) {
          currentState = ST_UNLOCKED;
          Serial.println("[STATE] -> ST_UNLOCKED (BLE Manual Unlock)");
        } else if (bits & FLAG_FINGER_TOUCHED) {
          currentState = ST_AUTH_VERIFYING;
          Serial.println("[STATE] -> ST_AUTH_VERIFYING");
        }
        break;
      }

      case ST_AUTH_VERIFYING: {                                                                   // AUTH_VERIFYING
        EventBits_t bits = xEventGroupWaitBits(systemEvents, FLAG_AUTH_PASSED | FLAG_AUTH_FAILED, 
                                               pdTRUE, pdFALSE, pdMS_TO_TICKS(6000));
        if (bits & FLAG_AUTH_PASSED) {
          currentState = ST_UNLOCKED;
          Serial.println("[STATE] -> ST_UNLOCKED");
        } else {
          Serial.println("[STATE] -> Verification timed out or failed. Returning to LOCKED.");
          vTaskDelay(pdMS_TO_TICKS(800));
          currentState = ST_LOCKED;
          arm_identify_mode();
        }
        break;
      }

      case ST_UNLOCKED: {                                                                       //UNLOCKED
        Serial.println("[SERVO] Unlocking latch...");
        walletServo.write(UNLOCKED_ANGLE);
        bleSetFlag(BLE_FLAG_CLEAR);
        vTaskDelay(pdMS_TO_TICKS(SERVO_TRAVEL_MS));

        uint32_t countdownStart = millis();
        while (millis() - countdownStart < UNLOCK_TIMEOUT_MS) {
          EventBits_t bits = xEventGroupGetBits(systemEvents);
          if (bits & FLAG_THEFT_DETECTED) {
            xEventGroupClearBits(systemEvents, FLAG_THEFT_DETECTED);
            currentState = ST_TAMPER_ALARM;
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(100));
        }

        Serial.println("[SERVO] Locking latch...");
        walletServo.write(LOCKED_ANGLE);
        vTaskDelay(pdMS_TO_TICKS(SERVO_TRAVEL_MS));

        vTaskDelay(pdMS_TO_TICKS(500));

        if (currentState != ST_TAMPER_ALARM) {
          currentState = ST_LOCKED;
          Serial.println("[STATE] -> ST_LOCKED (Ready for next scan)");
          arm_identify_mode();
        }
        break;
      }

      case ST_TAMPER_ALARM: {                                                             // TAMPER_ALARM
        Serial.println("[ALARM] Motion signature detected! Pushing alert...");
        const char* theftMsg = " The wallet detected a high-g snatch motion signature! ";
        xQueueSend(alertMessageQueue, &theftMsg, 0);

        for (int i = 0; i < 10; i++) {
          vTaskDelay(pdMS_TO_TICKS(250));
          EventBits_t bits = xEventGroupGetBits(systemEvents);
          if (bits & FLAG_AUTH_PASSED) {
            xEventGroupClearBits(systemEvents, FLAG_AUTH_PASSED);
            Serial.println("[ALARM] Authorized biometric override detected.");
            break;
          }
        }
        currentState = ST_LOCKED;
        arm_identify_mode();
        break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ============================================================================
// TASK: BIOMETRIC AUTH & COMMAND PROCESSOR (CORE 1)
// ============================================================================
void TaskBiometricAuth(void *pvParameters) {
  Serial1.setRxBufferSize(2048);
  Serial1.begin(921600, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  vTaskDelay(pdMS_TO_TICKS(200));

  while (Serial1.available()) { Serial1.read(); }

  if (!mySensor.begin(Serial1)) {
    Serial.println("[ERROR] FPC2534 did not respond. Retrying soft reset...");
    software_reset_sensor();
  }

  cmd_cb.on_error = on_error;
  cmd_cb.on_status = on_status;
  cmd_cb.on_enroll = on_enroll;
  cmd_cb.on_identify = on_identify;
  cmd_cb.on_list_templates = on_list_templates;
  cmd_cb.on_is_ready_change = on_is_ready_change;
  mySensor.setCallbacks(cmd_cb);

  software_reset_sensor();

  // Give sensor a moment to initialize before arming once
  uint32_t bootWait = millis();
  while (millis() - bootWait < 500) {
    if (Serial1.available() > 0) {
      mySensor.processNextResponse();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  arm_identify_mode();

  for (;;) {
    // 1. Process queued BLE management commands
    uint8_t incomingCmd = 0;
    if (xQueueReceive(bleCommandQueue, &incomingCmd, 0) == pdTRUE) {
      Serial.printf("[BLE CMD] Executing command: 0x%02X\n", incomingCmd);

      switch (incomingCmd) {
        case BLE_CMD_ENROLL_START: {
          Serial.println("[FPC] Preparing enrollment session...");
          bleSetFlag(BLE_FLAG_ENROLLING);

          // Reset to cancel background identify mode
          mySensor.sendReset();

          uint32_t resetWait = millis();
          while (millis() - resetWait < 600) {
            if (Serial1.available() > 0) {
              mySensor.processNextResponse();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
          }

          fpc_id_type_t id;
          id.type = ID_TYPE_GENERATE_NEW;
          id.id = 0;

          Serial.println("[FPC] Place and remove finger to enroll...");
          fpc_result_t rc = mySensor.requestEnroll(id);
          if (rc != FPC_RESULT_OK) {
            Serial.printf("[ERROR] Failed to start enroll - error: %d\n", rc);
            bleSetFlag(BLE_FLAG_CLEAR);
            arm_identify_mode();
          } else {
            Serial.println("[FPC] Enroll active! Tap and lift finger repeatedly...");
          }
          break;
        }

        case BLE_CMD_DELETE_ALL: {
          Serial.println("[FPC] Canceling background scan and preparing to delete all templates...");
          mySensor.sendReset();

          uint32_t resetWait = millis();
          while (millis() - resetWait < 600) {
            if (Serial1.available() > 0) {
              mySensor.processNextResponse();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
          }

          Serial.println("[FPC] Deleting all stored templates...");
          fpc_id_type_t id = {0};
          id.type = ID_TYPE_ALL;
          id.id = 0;

          fpc_result_t rc = mySensor.requestDeleteTemplate(id);
          if (rc != FPC_RESULT_OK) {
            Serial.printf("[ERROR] Failed to delete templates - error: %d\n", rc);
          } else {
            numberOfTemplates = 0;
            Serial.println("[FPC] All templates wiped successfully!");
          }
          arm_identify_mode();
          break;
        }

        case BLE_CMD_LIST_TEMPLATES: {
          Serial.println("[FPC] Querying stored template database...");
          mySensor.sendReset();
          vTaskDelay(pdMS_TO_TICKS(300));
          while (Serial1.available()) { mySensor.processNextResponse(); }

          mySensor.requestListTemplates();
          vTaskDelay(pdMS_TO_TICKS(300));
          while (Serial1.available()) { mySensor.processNextResponse(); }

          arm_identify_mode();
          break;
        }

        case BLE_CMD_SERVO_OPENING: {
          Serial.println("[BLE CMD] Manual servo unlock requested via BLE.");
          xEventGroupSetBits(systemEvents, FLAG_AUTH_PASSED);
          break;
        }

        default:
          Serial.printf("[BLE CMD] Unhandled opcode: 0x%02X\n", incomingCmd);
          break;
      }
    }

    // 2. Continually stream incoming UART frames from sensor
    if (Serial1.available() > 0) {
      mySensor.processNextResponse();
    }

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ============================================================================
// TASK: 6-DOF IMU MOTION TELEMETRY (CORE 0)
// ============================================================================
void TaskIMUTelemetry(void *pvParameters) {
  Wire.begin(IMU_SDA, IMU_SCL, 400000);
  vTaskDelay(pdMS_TO_TICKS(50));

  writeIMURegister(REG_CTRL1_XL, 0x40); // Accel: 104Hz, +/-4g
  vTaskDelay(pdMS_TO_TICKS(10));
  writeIMURegister(REG_CTRL2_G, 0x40);  // Gyro: 104Hz, +/-500 dps
  vTaskDelay(pdMS_TO_TICKS(50));

  float lastLinearMag = 1.0;
  uint32_t sampleCounter = 0;
  const float ACCEL_JERK_THRESHOLD = 0.75;
  const float GYRO_ROTATION_THRESHOLD = 180.0;

  for (;;) {
    Wire.beginTransmission(IMU_I2C_ADDR);
    Wire.write(REG_OUTX_L_G);
    if (Wire.endTransmission(false) == 0) {
      Wire.requestFrom(IMU_I2C_ADDR, 12);

      if (Wire.available() >= 12) {
        int16_t rawGyroX = Wire.read() | (Wire.read() << 8);
        int16_t rawGyroY = Wire.read() | (Wire.read() << 8);
        int16_t rawGyroZ = Wire.read() | (Wire.read() << 8);

        int16_t rawAccX = Wire.read() | (Wire.read() << 8);
        int16_t rawAccY = Wire.read() | (Wire.read() << 8);
        int16_t rawAccZ = Wire.read() | (Wire.read() << 8);

        float ax = rawAccX * 0.122 / 1000.0;
        float ay = rawAccY * 0.122 / 1000.0;
        float az = rawAccZ * 0.122 / 1000.0;
        float currentLinearMag = sqrt(ax * ax + ay * ay + az * az);
        float deltaJerk = abs(currentLinearMag - lastLinearMag);
        lastLinearMag = currentLinearMag;

        float gx = (rawGyroX * 17.50) / 1000.0;
        float gy = (rawGyroY * 17.50) / 1000.0;
        float gz = (rawGyroZ * 17.50) / 1000.0;
        float totalRotationVelocity = sqrt(gx * gx + gy * gy + gz * gz);

        if (sampleCounter % 100 == 0) {
          Serial.printf("[IMU] Force: %.2fG | Jerk: %.3fG | Rot: %.1f dps\n",
                        currentLinearMag, deltaJerk, totalRotationVelocity);
        }

        if (currentState == ST_LOCKED) {
          if (deltaJerk > ACCEL_JERK_THRESHOLD && totalRotationVelocity > GYRO_ROTATION_THRESHOLD) {
            Serial.println("[THEFT ALERT] Motion signature detected!");
            xEventGroupSetBits(systemEvents, FLAG_THEFT_DETECTED);
            bleSetFlag(BLE_FLAG_IMU_MOVING);
          }
        }
      }
    }
    sampleCounter++;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}