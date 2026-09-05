/**
 * @file    test/unit/fingerprint_uart/main.cpp
 * @brief   Unit test: FPC2534 fingerprint sensor over UART.
 *
 * Build and flash:
 *     pio run -e test_fingerprint_uart -t upload -t monitor
 *
 * This is the test that took longest to pass, and the reason the vendor library
 * in lib/ is vendored rather than pulled from the registry. Three hardware
 * facts make it harder than the vendor example:
 *
 *   - RST_N is strapped to 3V3, so there is no reset line to toggle. Recovery
 *     has to happen over the protocol.
 *   - CS_N / SYS_WU is strapped to 3V3, so the sensor never sleeps and the IRQ
 *     line is not a usable "finger present" edge.
 *   - The link runs at 921600 baud, and the sensor emits ASCII bootloader text
 *     before its first binary frame. A parser that assumes the stream starts on
 *     a frame boundary reads that text as a header and fails with error 33
 *     forever.
 *
 * The fix - scraping the stream byte by byte for a valid frame start, and
 * draining the RX ring before a reset rather than after - is described in
 * docs/04-fpc2534-uart-debugging.md.
 *
 * Interactive menu over the USB console:
 *     1) enroll a new fingerprint
 *     2) erase all templates
 *     3) validate a fingerprint
 *
 * PASS: the sensor reaches "ready", the template list comes back, an enrolment
 * completes in 12 samples, and a validate returns MATCH for an enrolled finger
 * and NO MATCH for any other.
 */

#include <Arduino.h>
#include "SparkFun_FPC2534.h"

// Pin definitions — custom PCB via ZIF connector (ESP32-S3-MINI-1)
// CS_N (SYS_WU) tied to 3.3V: sensor never enters deep sleep, no wake-up needed.
// RST_N tied to VCC: no hardware reset available, use mySensor.sendReset() instead.
#define IRQ_PIN 16
#define UART_TX_PIN 17
#define UART_RX_PIN 18

void software_reset_sensor(void);

uint16_t numberOfTemplates = 0;
SfeFPC2534UART mySensor;
bool isInitialized = false;
bool drawTheMenu = false;

// Add this near your other global flags at the top of main.cpp
bool triggerTemplateList = false;


//------------------------------------------------------------------------------------
// Menu
//------------------------------------------------------------------------------------
static void drawMenu()
{
    drawTheMenu = false;
    mySensor.setLED(false);

    Serial.println("\n--- FPC2534 Fingerprint Menu ---");
    Serial.print("Templates enrolled: ");
    Serial.print(numberOfTemplates);
    Serial.println(" / 30 max");
    Serial.println("Select option:");
    Serial.println("\t1) Enroll a new fingerprint");
    Serial.println("\t2) Erase all fingerprint templates");
    Serial.println("\t3) Validate a fingerprint");
    Serial.print("\n> ");

    Serial.flush();
    while (Serial.available() > 0)
        Serial.read();

    uint8_t chIn;
    while (true)
    {
        if (Serial.available() > 0)
        {
            chIn = Serial.read();
            if (chIn == '1' || chIn == '2' || chIn == '3')
            {
                Serial.println((char)chIn);
                break;
            }
            else
                Serial.write(7); // beep
        }
        delay(10);
    }
    Serial.println();

    if (chIn == '1')
    {
        Serial.println("Place and remove finger to enroll...");
        mySensor.setLED(true);
        fpc_id_type_t id;
        id.type = ID_TYPE_GENERATE_NEW;
        id.id = 0;
        fpc_result_t rc = mySensor.requestEnroll(id);
        if (rc != FPC_RESULT_OK)
        {
            Serial.print("[ERROR]\tFailed to start enroll - error: ");
            Serial.println(rc);
        }
        else
            Serial.print("\tsamples remaining 12..");
    }
    else if (chIn == '2')
    {
        if (numberOfTemplates == 0)
        {
            Serial.println("[INFO]\tNo templates to delete");
            drawTheMenu = true;
        }
        else
        {
            Serial.println("Deleting all templates...");
            fpc_id_type_t id = {0};
            id.type = ID_TYPE_ALL;
            id.id = 0;
            fpc_result_t rc = mySensor.requestDeleteTemplate(id);
            if (rc != FPC_RESULT_OK)
            {
                Serial.print("[ERROR]\tFailed to delete templates - error: ");
                Serial.println(rc);
            }
            else
                numberOfTemplates = 0;
        }
    }
    else if (chIn == '3')
    {
        if (numberOfTemplates == 0)
        {
            Serial.println("[INFO]\tNo templates to validate against");
            drawTheMenu = true;
        }
        else
        {
            Serial.print("Place finger to validate...");
            fpc_id_type_t id = {0};
            id.type = ID_TYPE_ALL;
            id.id = 0;
            fpc_result_t rc = mySensor.requestIdentify(id, 1);
            if (rc != FPC_RESULT_OK)
            {
                Serial.print("[ERROR]\tFailed to start identify - error: ");
                Serial.println(rc);
            }
        }
    }
}

//------------------------------------------------------------------------------------
// Callbacks
//------------------------------------------------------------------------------------

static void on_error(uint16_t error)
{
    Serial.print("[ERROR]\tSensor Error Code: ");
    Serial.println(error);
    software_reset_sensor();
}

static void on_is_ready_change(bool isReady)
{
    if (isReady)
    {
        Serial.println("[STARTUP]\tFPC2534 Device is ready");
        // Defer the execution to the main loop frame
        triggerTemplateList = true; 
    }
}

static void on_identify(bool is_match, uint16_t id)
{
    if (is_match)
    {
        Serial.print("MATCH  {Template ID: ");
        Serial.print(id);
        Serial.println("}");
    }
    else
        Serial.println("NO MATCH");
}

static void on_enroll(uint8_t feedback, uint8_t samples_remaining)
{
    if (samples_remaining == 0)
    {
        Serial.println("..done!");
        delay(500);
        numberOfTemplates++;
    }
    else
    {
        Serial.print(samples_remaining);
        Serial.print(".");
    }
}

static void on_list_templates(uint16_t num_templates, uint16_t *template_ids)
{
    numberOfTemplates = num_templates;
    isInitialized = true;
    drawTheMenu = true;
}

static void on_status(uint16_t event, uint16_t state)
{
    if (mySensor.currentMode() == 0)
    {
        // End of enroll or identify
        if (event == EVENT_FINGER_LOST)
            drawTheMenu = true;
        // Check if app firmware is ready (covers delete-all completion and idle)
        else if ((state & STATE_APP_FW_READY) == STATE_APP_FW_READY)
        {
            // Delete-all fires EVENT_NONE when complete
            if (event == EVENT_NONE)
                drawTheMenu = true;
            // Sensor returned to idle after initialization
            else if (event == EVENT_IDLE && isInitialized)
                drawTheMenu = true;
        }
    }
    else if (mySensor.currentMode() == STATE_ENROLL && event == EVENT_FINGER_LOST)
    {
        Serial.print(".");
    }
}

static sfDevFPC2534Callbacks_t cmd_cb = {0};

//------------------------------------------------------------------------------------
// Software reset (RST_N is tied to VCC — no hardware toggle available)
//------------------------------------------------------------------------------------
void software_reset_sensor(void)
{
    // Flush any stale data BEFORE sending reset
    while (Serial1.available())
        Serial1.read();
        
    mySensor.sendReset();
    
    // Give sensor time to boot — don't flush after, the boot status needs to arrive.
    // The library sliding synchronizer will safely discard the bootloader text now.
    delay(500);
}

//------------------------------------------------------------------------------------
// setup()
//------------------------------------------------------------------------------------
void setup()
{
    delay(2000);

    Serial.begin(921600);
    while (!Serial)
        ;

    Serial.println();
    Serial.println("--- FPC2534 Fingerprint Sensor ---");
    Serial.println("CS_N -> 3.3V (no deep sleep), RST_N -> VCC (software reset only)");
    Serial.println();

    // OPTIMIZATION: Expand the RX buffer buffer size to 2048.
    // 512 bytes is too tight for high-volume transactions at 921600 baud.
#if defined(ARDUINO_ARCH_RP2040)
    Serial1.setFIFOSize(1024);
#elif defined(ESP32)
    Serial1.setRxBufferSize(2048); 
#endif

    pinMode(IRQ_PIN, INPUT);

    Serial1.begin(921600, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    delay(500);
    for (uint32_t startMS = millis(); !Serial1 && (millis() - startMS < 5000);)
        delay(200);
    Serial.println("[STARTUP]\tSerial1 ready.");

    // With RST tied to VCC the sensor boots on power-up.
    delay(500);

    // Discard initial power-on noise cleanly before starting our managed session
    while (Serial1.available())
        Serial1.read();

    if (!mySensor.begin(Serial1))
    {
        Serial.println("[ERROR]\tFPC2534 not found. Check wiring. HALT.");
        while (1)
            delay(1000);
    }
    Serial.println("[STARTUP]\tFPC2534 initialized.");

    // Register callbacks
    cmd_cb.on_error = on_error;
    cmd_cb.on_status = on_status;
    cmd_cb.on_enroll = on_enroll;
    cmd_cb.on_identify = on_identify;
    cmd_cb.on_list_templates = on_list_templates;
    cmd_cb.on_is_ready_change = on_is_ready_change;
    mySensor.setCallbacks(cmd_cb);

    // Execute soft reset to initialize state machine
    software_reset_sensor();

    Serial.println("[STARTUP]\tWaiting for sensor ready status...");
}

//------------------------------------------------------------------------------------
// loop()
//------------------------------------------------------------------------------------
void loop()
{
    // Handle deferred startup actions outside of the callback stack context
    if (triggerTemplateList)
    {
        triggerTemplateList = false; // Reset flag immediately
        Serial.println("[STARTUP]\tRequesting enrolled template list...");
        
        fpc_result_t rc = mySensor.requestListTemplates();
        if (rc != FPC_RESULT_OK)
        {
            Serial.print("[ERROR]\tFailed to get template list - error: ");
            Serial.println(rc);
        }
    }

    // Process incoming responses
    fpc_result_t rc = mySensor.processNextResponse();
    
    if (rc == FPC_RESULT_IO_BAD_DATA)
    {
        Serial.println("[DEBUG]\tStream synchronization timed out or frame structural corruption occurred.");
    }
    else if (rc == FPC_RESULT_IO_NO_DATA || rc == FPC_PENDING_OPERATION)
    {
        // Normal data streaming state; wait for more bytes to land
        delay(1);
    }
    else if (rc != FPC_RESULT_OK)
    {
        Serial.print("[ERROR]\tProcessing Error: ");
        Serial.println(rc);
    }
    
    if (drawTheMenu)
    {
        drawMenu();
    }

    yield();
}