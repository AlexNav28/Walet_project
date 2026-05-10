/**
 * @file main.cpp
 * @brief FPC2534 Fingerprint Sensor - Enroll & Verify
 *
 * Simple program to enroll fingerprints and verify them on the FPC2534
 * using UART communication with an Adafruit Feather ESP32-S3.
 *
 * Wiring (Feather ESP32-S3 -> FPC2534 breakout):
 *   - SENSOR_TX_PIN -> FPC2534 RX
 *   - SENSOR_RX_PIN -> FPC2534 TX
 *   - RST_PIN       -> FPC2534 RST
 *   - 3.3V          -> FPC2534 VCC
 *   - GND           -> FPC2534 GND
 */

#include <Arduino.h>
#include "SparkFun_FPC2534.h"

// ============================================================================
// PIN DEFINITIONS - change these to match your wiring
// ============================================================================
#define SENSOR_TX_PIN  39  // ESP32-S3 TX -> Sensor RX
#define SENSOR_RX_PIN  38  // ESP32-S3 RX <- Sensor TX
#define RST_PIN 0
// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
void reset_sensor(void);
void drawMenu(void);
void handleChoice(char choice);

// ============================================================================
// GLOBALS
// ============================================================================
SfeFPC2534UART mySensor;

// How many fingerprints are stored on the sensor
uint16_t numberOfTemplates = 0;

// Flags to control program flow
bool deviceReady = false;
bool showMenu = false;

// ============================================================================
// CALLBACKS - the library calls these when the sensor sends events
// ============================================================================

// Called on sensor errors
static void on_error(uint16_t error)
{
    Serial.print("[ERROR]\tCode: ");
    Serial.println(error);
    reset_sensor();
}

// Called when sensor firmware finishes booting
static void on_is_ready_change(bool isReady)
{
    if (isReady)
    {
        Serial.println("[OK]\tSensor is ready");
        // Ask the sensor how many fingerprints it has stored
        mySensor.requestListTemplates();
    }
}

// Called when sensor returns the list of stored templates
static void on_list_templates(uint16_t num_templates, uint16_t *template_ids)
{
    numberOfTemplates = num_templates;
    deviceReady = true;
    showMenu = true;
}

// Called during enrollment with progress updates
static void on_enroll(uint8_t feedback, uint8_t samples_remaining)
{
    if (samples_remaining == 0)
    {
        // Enrollment complete
        Serial.println(" Done!");
        numberOfTemplates++;
        mySensor.setLED(false);
    }
    else
    {
        // Still need more finger touches
        Serial.print(" ");
        Serial.print(samples_remaining);
        Serial.print(" left..");
    }
}

// Called when identification (verify) finishes
static void on_identify(bool is_match, uint16_t id)
{
    if (is_match)
    {
        Serial.print("[MATCH]\tFingerprint recognized! Template ID: ");
        Serial.println(id);
    }
    else
    {
        Serial.println("[NO MATCH]\tFingerprint not recognized");
    }
    mySensor.setLED(false);
}

// Called on every status event — used to know when to show menu again
static void on_status(uint16_t event, uint16_t state)
{
    // Show menu when sensor returns to idle after an operation
    if (deviceReady && mySensor.currentMode() == 0)
    {
        if (event == EVENT_FINGER_LOST || event == EVENT_IDLE || event == EVENT_NONE)
            showMenu = true;
    }
}

// Callback struct
static sfDevFPC2534Callbacks_t callbacks = {0};

// ============================================================================
// HELPERS
// ============================================================================

// Software reset — sends reset command over UART (no RST pin needed)
void reset_sensor(void)
{
    pinMode(RST_PIN, OUTPUT);
    digitalWrite(RST_PIN, LOW);
    delay(10);
    digitalWrite(RST_PIN, HIGH);
    delay(300);
}

// Print the menu and wait for user input via Serial Monitor
void drawMenu()
{
    showMenu = false;
    mySensor.setLED(false);

    Serial.println();
    Serial.println("========================================");
    Serial.print("  Stored fingerprints: ");
    Serial.print(numberOfTemplates);
    Serial.println(" / 30");
    Serial.println("========================================");
    Serial.println("  1) Enroll new fingerprint");
    Serial.println("  2) Verify fingerprint");
    Serial.println("  3) Delete all fingerprints");
    Serial.println("========================================");
    Serial.print("> ");

    // Wait for user to type 1, 2, or 3
    while (true)
    {
        if (Serial.available() > 0)
        {
            char ch = Serial.read();
            if (ch == '1' || ch == '2' || ch == '3')
            {
                Serial.println(ch);
                handleChoice(ch);
                return;
            }
        }
        // Keep processing sensor messages while waiting for input
        mySensor.processNextResponse();
        delay(10);
    }
}

// Execute the user's menu choice
void handleChoice(char choice)
{
    if (choice == '1')
    {
        // --- ENROLL ---
        Serial.println("\n  Place finger on sensor repeatedly to enroll...");
        mySensor.setLED(true);

        fpc_id_type_t id;
        id.type = ID_TYPE_GENERATE_NEW;  // Let sensor assign the next ID
        id.id = 0;

        fpc_result_t rc = mySensor.requestEnroll(id);
        if (rc != FPC_RESULT_OK)
        {
            Serial.print("[ERROR]\tEnroll failed, code: ");
            Serial.println(rc);
        }
        else
        {
            Serial.print("  Touches remaining: 12..");
        }
    }
    else if (choice == '2')
    {
        // --- VERIFY ---
        if (numberOfTemplates == 0)
        {
            Serial.println("\n  No fingerprints enrolled yet!");
            showMenu = true;
            return;
        }

        Serial.println("\n  Place finger on sensor to verify...");
        mySensor.setLED(true);

        fpc_id_type_t id;
        id.type = ID_TYPE_ALL;  // Compare against all stored templates
        id.id = 0;

        fpc_result_t rc = mySensor.requestIdentify(id, 1);
        if (rc != FPC_RESULT_OK)
        {
            Serial.print("[ERROR]\tVerify failed, code: ");
            Serial.println(rc);
        }
    }
    else if (choice == '3')
    {
        // --- DELETE ALL ---
        if (numberOfTemplates == 0)
        {
            Serial.println("\n  Nothing to delete!");
            showMenu = true;
            return;
        }

        Serial.println("\n  Deleting all fingerprints...");
        fpc_id_type_t id;
        id.type = ID_TYPE_ALL;
        id.id = 0;

        fpc_result_t rc = mySensor.requestDeleteTemplate(id);
        if (rc != FPC_RESULT_OK)
        {
            Serial.print("[ERROR]\tDelete failed, code: ");
            Serial.println(rc);
        }
        else
        {
            numberOfTemplates = 0;
            Serial.println("  All fingerprints deleted.");
        }
    }
}

// ============================================================================
// SETUP
// ============================================================================
void setup()
{
    delay(2000);

    Serial.begin(115200);
    while (!Serial)
        delay(10);

    Serial.println();
    Serial.println("========================================");
    Serial.println("  FPC2534 - Enroll & Verify");
    Serial.println("  Adafruit Feather ESP32-S3");
    Serial.println("========================================");
    Serial.println();

    // Configure Serial1 for sensor communication
    Serial1.setRxBufferSize(512);
    Serial1.begin(921600, SERIAL_8N1, SENSOR_RX_PIN, SENSOR_TX_PIN);
    delay(100);
    for (uint32_t t = millis(); !Serial1 && (millis() - t < 5000);)
        delay(200);

    Serial.println("[INIT]\tSerial1 started (921600 baud)");

    // Initialize library
    if (!mySensor.begin(Serial1))
    {
        Serial.println("[ERROR]\tFPC2534 init failed - check wiring!");
        while (true) delay(1000);
    }
    Serial.println("[INIT]\tFPC2534 initialized");

    // Register callbacks
    callbacks.on_error = on_error;
    callbacks.on_status = on_status;
    callbacks.on_enroll = on_enroll;
    callbacks.on_identify = on_identify;
    callbacks.on_list_templates = on_list_templates;
    callbacks.on_is_ready_change = on_is_ready_change;
    mySensor.setCallbacks(callbacks);

    // Software reset — tells the sensor to reboot over UART
    // This triggers the boot status message → on_is_ready_change callback
    Serial.println("[INIT]\tSending software reset to sensor...");
    reset_sensor();
    Serial.println("[INIT]\tWaiting for sensor to boot...");
}

// ============================================================================
// LOOP
// ============================================================================
void loop()
{
    // Process incoming sensor messages
    fpc_result_t rc = mySensor.processNextResponse();

    if (rc != FPC_RESULT_OK && rc != FPC_PENDING_OPERATION)
    {
        Serial.print("[ERROR]\tResponse error: ");
        Serial.println(rc);
    }

    // Show menu when ready
    if (showMenu)
        drawMenu();

    delay(50);
}
