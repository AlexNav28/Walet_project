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



#define SERVICE_UUID        "4FAFC201-1FB5-459E-8FCC-C5C9C331914B"
#define STATUS_CHAR_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define COMMAND_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a9" 

#define BLE_FLAG_CLEAR         0x00
#define BLE_FLAG_WRONG_FINGER  0x01
#define BLE_FLAG_IMU_MOVING    0x02
#define BLE_FLAG_ENROLLING     0x04  // New: visible feedback during enroll

// BLE Commands (single byte sent from phone/PC)
#define CMD_ENROLL_START    0x01
#define CMD_DELETE_LAST     0x02
#define CMD_DELETE_ALL      0x03
#define CMD_LIST_TEMPLATES  0x04

BLECharacteristic* pStatusChar;
BLECharacteristic* pCommandChar;

QueueHandle_t bleCommandQueue;

// Hardware Pin Configurations from your Custom PCB
#define IMU_SDA         36
#define IMU_SCL         37
#define SERVO_PIN       1   
//#define BUZZER_PIN      0 
#define IRQ_PIN         16  
#define UART_TX_PIN     17  
#define UART_RX_PIN     18  

#define IMU_I2C_ADDR    0x6A
#define REG_CTRL1_XL    0x10
#define REG_OUTX_L_A    0x28


const int LOCKED_ANGLE = 180;
const int UNLOCKED_ANGLE = 120;
#define SERVO_OPEN_ANGLE   UNLOCKED_ANGLE
#define SERVO_CLOSE_ANGLE  LOCKED_ANGLE
#define UNLOCK_TIMEOUT_MS  30000  

// FreeRTOS Communication Events
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

Servo walletServo;
SfeFPC2534UART mySensor;
static sfDevFPC2534Callbacks_t cmd_cb = {0};

uint16_t numberOfTemplates = 0;
bool isInitialized = false; 
volatile bool scanArmed = false;

#define RSSI_THRESHOLD -65
#define MISSED_SCANS_BEFORE_ALERT 3
 
BLEScan* pBLEScan;
int missedScans = 0;
bool isMissing = false;
unsigned long lastAlertTime = 0;
int lastRSSI = -999;
 
BLEUUID targetUUID("4FAFC201-1FB5-459E-8FCC-C5C9C331914B");



void software_reset_sensor(void);
void writeIMURegister(byte reg, byte value);
void TaskSystemManager(void *pvParameters);
void TaskIMUTelemetry(void *pvParameters);
void TaskBiometricAuth(void *pvParameters);


void connectWiFi() {
  WiFi.disconnect(true);
  delay(1000);
  WiFi.mode(WIFI_STA);
  delay(500);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to hotspot");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
  } else {
    Serial.println("\nFailed to connect!");
  }
}

void sendDiscordAlert(String message) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  WiFiClientSecure *client = new WiFiClientSecure;
  if (client) {
    client->setInsecure();
    HTTPClient https;
    if (https.begin(*client, String(DISCORD_WEBHOOK))) {
      https.addHeader("Content-Type", "application/json");
      String payload = "{\"embeds\":[{"
        "\"title\":\"🚨 Smart Wallet Alert!\","
        "\"description\":\"" + message + "\","
        "\"color\":16711680"
      "}]}";
      int httpCode = https.POST(payload);
      Serial.print("Discord response: ");
      Serial.println(httpCode);
      https.end();
    }
    delete client;
  }
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.haveServiceUUID()) {
      if (advertisedDevice.isAdvertisingService(targetUUID)) {
        lastRSSI = advertisedDevice.getRSSI();
        Serial.print("Device found! RSSI: ");
        Serial.println(lastRSSI);
      }
    }
  }
};
 

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) {
    uint8_t cmd = pChar->getValue()[0];
    Serial.printf("[BLE CMD] Received command: 0x%02X\n", cmd);
    xQueueSend(bleCommandQueue, &cmd, 0);  // Forward to biometric task
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    Serial.println("[BLE] Phone connected.");
  }

  void onDisconnect(BLEServer* pServer) {
    Serial.println("[BLE] Phone disconnected — may be out of range!");
    // Restart advertising so phone can reconnect when back in range
    BLEDevice::getAdvertising()->start();
    
    // Trigger your Discord alert here
    sendDiscordAlert("⚠️ Wallet may be out of range — phone disconnected!");
  }
};

void initBLE() {
  BLEDevice::init("SmartWallet");
  BLEServer* pServer = BLEDevice::createServer();
  BLEService* pService = pServer->createService(SERVICE_UUID);
  pServer->setCallbacks(new ServerCallbacks());

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

void bleSetFlag(uint8_t flag) {
  uint8_t val = flag;
  pStatusChar->setValue(&val, 1);
  pStatusChar->notify();
}

// ============================================================================
// SYSTEM CORE SETUP
// ============================================================================
void setup() {

  Serial.begin(115200);
  delay(1000);
  connectWiFi();
  BLEDevice::init("SmartWallet");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  Serial.println("Smart Wallet ready. Scanning...");

  Serial.begin(921600);


  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  // REMOVE pinMode and digitalWrite for SERVO_PIN — attach() owns this pin now
  pinMode(IRQ_PIN, INPUT);       // Keep this
  // pinMode(SERVO_PIN, OUTPUT); // DELETE this
  // digitalWrite(SERVO_PIN, LOW); // DELETE this

  walletServo.setPeriodHertz(50);
  walletServo.attach(SERVO_PIN, 500, 2400);
  walletServo.write(LOCKED_ANGLE);
  delay(500);

  // USB CDC Connection Guard
  uint32_t serialTimeout = millis();
  while (!Serial && (millis() - serialTimeout < 4000)) {
    delay(10);
  }

  Serial.println("\n==================================================");
  Serial.println("[BOOT] Smart Wallet Multi-Core System Live.");
  Serial.println("==================================================");

  //pinMode(BUZZER_PIN, OUTPUT);
  //digitalWrite(BUZZER_PIN, LOW);
  //pinMode(SERVO_PIN, OUTPUT);
  //digitalWrite(SERVO_PIN, LOW);
  pinMode(IRQ_PIN, INPUT);

  bleCommandQueue = xQueueCreate(5, sizeof(uint8_t));
 
  initBLE();

  systemEvents = xEventGroupCreate();

  // Spawning matching priority tasks to force balanced round-robin scheduling
  xTaskCreatePinnedToCore(TaskSystemManager, "SysManager", 8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskBiometricAuth, "BioAuth", 16384, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskIMUTelemetry, "IMU_Track", 4096, NULL, 2, NULL, 0);

  Serial.println("[BOOT] All tasks distributed to scheduler kernels.\n");
}

void loop() {
  // Yield main thread loop
}

// ============================================================================
// FPC2534 RE-ALIGNED CALLBACK INTERFACES
// ============================================================================
static void on_error(uint16_t error) {
  Serial.printf("[CALLBACK ERROR] Sensor reported Error Code: 0x%04X\n", error);
  scanArmed = false; // FIX: Force re-arming sequence after recovery finishes
  software_reset_sensor();
}

static void on_is_ready_change(bool isReady) {
  Serial.printf("[CALLBACK READY] Sensor Operation Status: %s\n", isReady ? "READY" : "NOT_READY");
}

static void on_identify(bool is_match, uint16_t id) {
  if (is_match) {
    Serial.printf("[CALLBACK AUTH] MATCH found! Template ID: %d\n", id);
    xEventGroupSetBits(systemEvents, FLAG_AUTH_PASSED);
    bleSetFlag(BLE_FLAG_CLEAR);   
  } else {
    Serial.println("[CALLBACK AUTH] NO MATCH. Access Denied.");
    xEventGroupSetBits(systemEvents, FLAG_AUTH_FAILED);
    bleSetFlag(BLE_FLAG_WRONG_FINGER); 
  }
}

static void on_enroll(uint8_t feedback, uint8_t samples_remaining) {
Serial.printf("[ENROLL] Feedback: %d | Samples remaining: %d\n", feedback, samples_remaining);
  if (samples_remaining == 0) {
    Serial.println("[ENROLL] Complete! Re-syncing template count.");
    bleSetFlag(BLE_FLAG_CLEAR);
    mySensor.requestListTemplates();  // Refresh numberOfTemplates

}
}

static void on_list_templates(uint16_t num_templates, uint16_t *template_ids) {
  numberOfTemplates = num_templates;
  isInitialized = true; 
  Serial.printf("[CALLBACK SYNC] Database Sync Complete. Templates Enrolled: %d\n", numberOfTemplates);
}

static void on_status(uint16_t event, uint16_t state) {
  Serial.printf("[CALLBACK STATUS] Raw Event: %d | Raw State Bitmap: 0x%04X\n", event, state);

  if (event == EVENT_FINGER_DETECT || (state & STATE_FINGER_DOWN)) {
    Serial.println("  -> Sensor status line reports: Finger Present.");
    if (currentState == ST_LOCKED) {
      xEventGroupSetBits(systemEvents, FLAG_FINGER_TOUCHED);
    }
  }
}

void software_reset_sensor(void) {
  Serial.println("[UART PROCESS] Sending software reset packet sequence...");
  while (Serial1.available()) { Serial1.read(); }
  mySensor.sendReset();
  vTaskDelay(pdMS_TO_TICKS(500)); 
}

void writeIMURegister(byte reg, byte value) {
  Wire.beginTransmission(IMU_I2C_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

// ============================================================================
// CORE 1: MAIN SYSTEM COORDINATOR TASK
// ============================================================================
void TaskSystemManager(void *pvParameters) {
  Serial.println("[TASK START] System Manager operational on Core 1.");

  for (;;) {
    Serial.printf("[DEBUG] Current State = %d\n", currentState);
    switch (currentState) {
      case ST_LOCKED: {
        EventBits_t bits = xEventGroupWaitBits(systemEvents, FLAG_FINGER_TOUCHED | FLAG_THEFT_DETECTED, pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & FLAG_THEFT_DETECTED) {
          currentState = ST_TAMPER_ALARM;
          Serial.println("[STATE CHANGE] -> Entering TAMPER_ALARM state!");
        } else if (bits & FLAG_FINGER_TOUCHED) {
          currentState = ST_AUTH_VERIFYING;
          Serial.println("[STATE CHANGE] -> Entering AUTH_VERIFYING state.");
        }
        break;
      }
      case ST_AUTH_VERIFYING: {
        EventBits_t bits = xEventGroupWaitBits(systemEvents, FLAG_AUTH_PASSED | FLAG_AUTH_FAILED, pdTRUE, pdFALSE, pdMS_TO_TICKS(6000));
        if (bits & FLAG_AUTH_PASSED) {
          currentState = ST_UNLOCKED;
          Serial.println("[STATE CHANGE] -> Entering UNLOCKED state.");
        } else {
          Serial.println("[STATE CHANGE] -> Verification window closed. Returning to LOCKED.");
          
          // FIX: Non-blocking delay gives the user time to lift their finger 
          // and prevents UART bus command collisions.
          vTaskDelay(pdMS_TO_TICKS(800)); 
          
          currentState = ST_LOCKED;
        }
        break;
      }
      case ST_UNLOCKED: {
        Serial.println("[SERVO] Unlocking...");
        walletServo.write(UNLOCKED_ANGLE);
        bleSetFlag(BLE_FLAG_CLEAR);
        vTaskDelay(pdMS_TO_TICKS(800));


        
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
          Serial.println("[SERVO] Locking...");
          walletServo.write(LOCKED_ANGLE);

          vTaskDelay(pdMS_TO_TICKS(800));

          walletServo.detach();
          digitalWrite(SERVO_PIN, LOW);

        if (currentState != ST_TAMPER_ALARM) {
          currentState = ST_LOCKED;
          Serial.println("[STATE CHANGE] -> Lock sequence completed. Returning to LOCKED.");
        }
        break;
      }
      case ST_TAMPER_ALARM: {
        Serial.println("[ALARM STATUS] Local audio siren active! Firing bounded 10-beep alert cycle...");
        bool masterOverrideCaught = false;

        // FIXED LOGIC: Caps total alerts to exactly 10 cycles to protect battery health
        for (int alertCount = 1; alertCount <= 10; alertCount++) {
          Serial.printf("[ALARM SIREN] Generating pulse pulse burst %d / 10\n", alertCount);
          
          //digitalWrite(BUZZER_PIN, HIGH);
          vTaskDelay(pdMS_TO_TICKS(150));
          //digitalWrite(BUZZER_PIN, LOW);
          vTaskDelay(pdMS_TO_TICKS(100));

          // Read the event flags mid-flight to catch an intentional biometric unlock bypass
          EventBits_t bits = xEventGroupGetBits(systemEvents);
          if (bits & FLAG_AUTH_PASSED) {
            xEventGroupClearBits(systemEvents, FLAG_AUTH_PASSED);
            masterOverrideCaught = true;
            Serial.println("[STATE CHANGE] -> Master override verified mid-alarm. Silencing buzzer.");
            break;
          }
        }

        // Automatic exit processing sequence
        currentState = ST_LOCKED;
        if (!masterOverrideCaught) {
          Serial.println("[STATE CHANGE] -> Bounded 10-beep ceiling hit. Automatically auto-rearming wallet to LOCKED.");
        }
        break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10)); 
  }
}

// ============================================================================
// CORE 1: BIOMETRIC DRIVER ENGINE (FIXED BINDINGS)
// ============================================================================
void TaskBiometricAuth(void *pvParameters) {
  Serial1.setRxBufferSize(2048); 
  Serial1.begin(921600, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  vTaskDelay(pdMS_TO_TICKS(200)); 

  while (Serial1.available()) { Serial1.read(); }

  if (!mySensor.begin(Serial1)) {
    Serial.println("[CRITICAL ERROR] FPC2534 initialization failed! Halted.");
    for(;;) { vTaskDelay(portMAX_DELAY); }
  }

  // Populate callback vectors cleanly
  cmd_cb.on_error = on_error;
  cmd_cb.on_status = on_status;
  cmd_cb.on_enroll = on_enroll;
  cmd_cb.on_identify = on_identify;
  cmd_cb.on_list_templates = on_list_templates;
  cmd_cb.on_is_ready_change = on_is_ready_change;
  mySensor.setCallbacks(cmd_cb);

  software_reset_sensor();
  
  // Sync your database array at boot
  mySensor.requestListTemplates();
  vTaskDelay(pdMS_TO_TICKS(50));

  for (;;) {
    // ========================================================================
    // FIX: ACTIVE BIOMETRIC ARMING LAYER
    // ========================================================================
    // Instead of waiting for a passive digitalRead trace loop, we command the 
    // sensor to actively monitor its surface the moment the wallet is LOCKED.
    if (currentState == ST_LOCKED && !scanArmed) {
      scanArmed = true;
      Serial.println("[BIOMETRIC] Wallet armed. Powering up capacitive array for scans...");
      
      fpc_id_type_t id = {0};
      id.type = ID_TYPE_ALL;
      id.id = 0;
      
      fpc_result_t rc = mySensor.requestIdentify(id, 1);
      if (rc != FPC_RESULT_OK) {
        Serial.printf("[UART ERROR] Failed to arm scanner engine. Code: %d\n", rc);
        scanArmed = false;
      }
    }

    // Reset our arming trigger whenever the wallet changes states (unblocks or alarms)
    if (currentState != ST_LOCKED) {
      scanArmed = false;
    }

    // Process parsed responses continuously as bytes stream into the buffer
    if (Serial1.available() > 0) {
      mySensor.processNextResponse();
    }
    

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// ============================================================================
// CORE 0: TELEMETRY ENGINE (CORE-LOCAL I2C ALLOCATION)
// ============================================================================
// Additional Register Pointer definitions for your IMU configuration
#define REG_CTRL2_G     0x11
#define REG_OUTX_L_G    0x22

void TaskIMUTelemetry(void *pvParameters) {
  Serial.println("[TASK START] 6-DoF IMU Telemetry Engine operational on Core 0.");
  
  Wire.begin(IMU_SDA, IMU_SCL, 400000);
  vTaskDelay(pdMS_TO_TICKS(50));
  
  // 1. Initialize Peripherals
  writeIMURegister(REG_CTRL1_XL, 0x40); // Turn on Accelerometer (104Hz, +/-4g Full Scale)
  vTaskDelay(pdMS_TO_TICKS(10));
  writeIMURegister(REG_CTRL2_G, 0x40);  // Turn on Gyroscope (104Hz, +/-500 dps Full Scale)
  vTaskDelay(pdMS_TO_TICKS(50));
  Serial.println("[IMU LINK] Dual-engine multi-axis streaming active on Core 0.");

  float lastLinearMag = 1.0;
  uint32_t sampleCounter = 0;

  // Multi-Axis Performance Threshold Limits
  const float ACCEL_JERK_THRESHOLD = 0.75; // Linear force change boundary (G-force)
  const float GYRO_ROTATION_THRESHOLD = 180.0; // Rotational velocity boundary (Degrees/Second)

  for (;;) {
    // 2. High-Efficiency 12-Byte Block Read Transaction
    // We begin reading at the Gyro output registers (0x22). The IMU automatically 
    // increments its internal memory pointer over the wire into the Accel registers (0x28).
    Wire.beginTransmission(IMU_I2C_ADDR);
    Wire.write(REG_OUTX_L_G); 
    if (Wire.endTransmission(false) == 0) {
      Wire.requestFrom(IMU_I2C_ADDR, 12); // Grab all 12 data bytes sequentially
      
      if (Wire.available() >= 12) {
        // Parse Gyroscope Raw Data Strings
        int16_t rawGyroX = Wire.read() | (Wire.read() << 8);
        int16_t rawGyroY = Wire.read() | (Wire.read() << 8);
        int16_t rawGyroZ = Wire.read() | (Wire.read() << 8);

        // Parse Accelerometer Raw Data Strings
        int16_t rawAccX = Wire.read() | (Wire.read() << 8);
        int16_t rawAccY = Wire.read() | (Wire.read() << 8);
        int16_t rawAccZ = Wire.read() | (Wire.read() << 8);

        // 3. Mathematical Vector Scaler Processing
        // Accelerometer Math (+/-4g configuration mapping)
        float ax = rawAccX * 0.122 / 1000.0;
        float ay = rawAccY * 0.122 / 1000.0;
        float az = rawAccZ * 0.122 / 1000.0;
        float currentLinearMag = sqrt(ax*ax + ay*ay + az*az);
        float deltaJerk = abs(currentLinearMag - lastLinearMag);
        lastLinearMag = currentLinearMag;

        // Gyroscope Math (+/-500 dps configuration scale factor mapping = 17.50 mdps/LSB)
        float gx = (rawGyroX * 17.50) / 1000.0;
        float gy = (rawGyroY * 17.50) / 1000.0;
        float gz = (rawGyroZ * 17.50) / 1000.0;
        float totalRotationVelocity = sqrt(gx*gx + gy*gy + gz*gz); // Angular Velocity Magnitude

        // 4. Multi-Axis Live Logger (Outputs values every 500ms)
        if (sampleCounter % 50 == 0) {
          Serial.printf("[IMU 6-DoF] Linear Force: %.2fG | Jerk: %.3fG | Rotational Speed: %.1f dps\n", 
                        currentLinearMag, deltaJerk, totalRotationVelocity);
        }

        // 5. Intelligent Multi-Feature Theft Evaluation Guard
        // Theft signature criteria: Crosses BOTH an acceleration jerk event 
        // AND a high-rate angular rotation spin concurrently.
        if (currentState == ST_LOCKED) {
          if (deltaJerk > ACCEL_JERK_THRESHOLD && totalRotationVelocity > GYRO_ROTATION_THRESHOLD) {
            Serial.printf("[THEFT ALERT] Multi-Feature Match! Jerk: %.3fG | Rotation: %.1f dps\n", 
                          deltaJerk, totalRotationVelocity);
            xEventGroupSetBits(systemEvents, FLAG_THEFT_DETECTED);
            bleSetFlag(BLE_FLAG_IMU_MOVING); 
          }
        }
      }
    }
    sampleCounter++;
    vTaskDelay(pdMS_TO_TICKS(10)); // 10ms processing windows
  }
}
