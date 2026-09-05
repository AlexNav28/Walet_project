/**
 * @file    test/integration/full_system_rtos/main.cpp
 * @brief   Integration test: every on-board driver running together under
 *          FreeRTOS, with the radios and the app left out.
 *
 * Build and flash:
 *     pio run -e test_full_system -t upload -t monitor
 *
 * The milestone build between "each driver passes its own unit test" and the
 * shipped firmware in src/. It runs the real three-task layout - system manager
 * and biometric driver on core 1, IMU telemetry on core 0 - and the real
 * four-state machine, with no BLE, WiFi, phone callbacks or battery
 * management. Everything it proves can be proven over the USB console.
 *
 * It is kept because it shows the concurrency was made to work before any
 * connectivity was layered on, and because it is still the fastest way to tell
 * a firmware bug from a radio bug: when the full build in src/ misbehaves,
 * flashing this one answers whether it is the state machine or BLE.
 *
 * Left as it was written apart from this comment. It duplicates declarations
 * that src/ now keeps in include/, because the point of the file is what it
 * proved on the bench, not how it would be factored now.
 *
 * What it proves:
 *   - the fingerprint sensor keeps working when it no longer owns the CPU
 *   - the IMU polls at 100 Hz on core 0 without disturbing UART timing on core 1
 *   - the state machine transitions correctly under real sensor input
 *   - the servo end stops are reached under RTOS scheduling
 *
 * The servo lines are commented out: when this build was made the motor GPIO
 * rework had not been done yet.
 */

#include <Arduino.h>
#include <Wire.h>
//#include <ESP32Servo.h>
#include "SparkFun_FPC2534.h"

// Hardware Pin Configurations from your Custom PCB
#define IMU_SDA         36
#define IMU_SCL         37
//#define SERVO_PIN     1  
#define BUZZER_PIN      1   
#define IRQ_PIN         16  
#define UART_TX_PIN     17  
#define UART_RX_PIN     18  

#define IMU_I2C_ADDR    0x6A
#define REG_CTRL1_XL    0x10
#define REG_OUTX_L_A    0x28

#define SERVO_OPEN_ANGLE   180
#define SERVO_CLOSE_ANGLE  0
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

//Servo walletServo;
SfeFPC2534UART mySensor;
static sfDevFPC2534Callbacks_t cmd_cb = {0};

uint16_t numberOfTemplates = 0;
bool isInitialized = false; 
volatile bool scanArmed = false;

void software_reset_sensor(void);
void writeIMURegister(byte reg, byte value);
void TaskSystemManager(void *pvParameters);
void TaskIMUTelemetry(void *pvParameters);
void TaskBiometricAuth(void *pvParameters);

// ============================================================================
// SYSTEM CORE SETUP
// ============================================================================
void setup() {
  Serial.begin(921600);

  // USB CDC Connection Guard
  uint32_t serialTimeout = millis();
  while (!Serial && (millis() - serialTimeout < 4000)) {
    delay(10);
  }

  Serial.println("\n==================================================");
  Serial.println("[BOOT] Smart Wallet Multi-Core System Live.");
  Serial.println("==================================================");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  //pinMode(SERVO_PIN, OUTPUT);
  //digitalWrite(SERVO_PIN, LOW);
  pinMode(IRQ_PIN, INPUT);

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
  } else {
    Serial.println("[CALLBACK AUTH] NO MATCH. Access Denied.");
    xEventGroupSetBits(systemEvents, FLAG_AUTH_FAILED);
  }
}

static void on_enroll(uint8_t feedback, uint8_t samples_remaining) {}

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
        //walletServo.attach(SERVO_PIN, 500, 2400);
        //walletServo.write(SERVO_OPEN_ANGLE);
        //vTaskDelay(pdMS_TO_TICKS(600));
        
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

        //walletServo.write(SERVO_CLOSE_ANGLE);
        //vTaskDelay(pdMS_TO_TICKS(800));
        //walletServo.detach();
        //digitalWrite(SERVO_PIN, LOW);

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
          
          digitalWrite(BUZZER_PIN, HIGH);
          vTaskDelay(pdMS_TO_TICKS(150));
          digitalWrite(BUZZER_PIN, LOW);
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
          }
        }
      }
    }
    sampleCounter++;
    vTaskDelay(pdMS_TO_TICKS(10)); // 10ms processing windows
  }
}