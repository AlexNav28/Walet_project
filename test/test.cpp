/*
 *---------------------------------------------------------------------------------
 *
 * Copyright (c) 2025, SparkFun Electronics Inc.
 *
 * SPDX-License-Identifier: MIT
 *
 *---------------------------------------------------------------------------------
 */

/*
 * Example using the SparkFun FPC2534 Fingerprint sensor library to demonstrate the fingerprint
 * enrollment and identification of the sensor.  The example provides the user with the following options:
 *    - Enroll a new fingerprint
 *    - Delete all existing fingerprints
 *    - Validate a fingerprint
 *
 * This version of this example uses the I2C interface to communicate with the sensor.
 *
 * Example Setup:
 *   - Connect the SparkFun Qwiic FPC2534 Fingerprint sensor to your microcontroller using a qwiic cable.
 *       NOTE: Due to for structure of I2C communications implemented by the FPC2534 sensor, only
 *             ESP32 and Raspberry Pi RP2 microcontrollers are supported by this Arduino Library.
 *  - Connect the RST pin on the sensor to a digital pin on your microcontroller. This is used by the
 *    example to "reset the sensor" on startup.
 *  - Connect the IRQ pin on the sensor to a digital pin on your microcontroller. The sensor triggers
 *    an interrupt on this pin when it has data to send.
 *  - Update the IRQ_PIN and RST_PIN defines below to match the pins you are using.
 *
 * Operation:
 *  - On startup, the example "pings" the sensor address to verify it is present on the I2C bus.
 *  - If the sensor is found, the example initializes the sensor library and resets the sensor.
 *    NOTE: A reset appears to be needed after the I2C ping is sent.
 *  - The sensor object is initialized with the sensor address, interrupt pin, and the Wire object.
 *    NOTE: The I2C bus number is also provided, to allow the library to perform the low-level I2C commands
 *          required to read data from the sensor. This is needed due to the way the FPC2534 implements I2C.
 *  - The example registers several callback functions with the sensor library. These functions are called as
 *    messages are received from the sensor.
 *  - Once running, the example prevents a menu with the following options:
 *      1) Enroll a new fingerprint
 *      2) Erase all existing fingerprint templates
 *      3) Validate a fingerprint
 *
 *   Once an optoin is selected (enter the menu number), the example performs the requested operation.
 *
 *  - When registering a new fingerprint, the example prompts the user to place and remove their finger repeatedly
 *    until the fingerprint is fully enrolled. The example prints out the number of samples remaining as
 *    reported by the sensor. Normally 12 samples are needed to fully enroll a fingerprint.
 *  - When deleting all fingerprints, the example sends the delete command to the sensor. If successful,
 *    the example resets its internal count of enrolled fingerprints to zero.
 *  - When validating a fingerprint, the example prompts the user to place their finger on the sensor.
 *    If the fingerprint matches an enrolled fingerprint, the example prints out the ID of the matched
 *    fingerprint template.
 *
 *---------------------------------------------------------------------------------
 */

#include <Arduino.h>
#include <Wire.h>

#include "SparkFun_FPC2534.h"

//----------------------------------------------------------------------------
// User Config -
//----------------------------------------------------------------------------
// UPDATE THESE DEFINES TO MATCH YOUR HARDWARE SETUP
//
// These are the pins the IRQ and RST pins of the sensor are connected to the microcontroller.
//
// NOTE: The IRQ pin must be an interrupt-capable pin on your microcontroller
//
// Example pins for various SparkFun boards:
//
// ESP32 thing plus
// #define IRQ_PIN 16
// #define RST_PIN 21
// #define I2C_BUS 0

// ESP32 thing plus C
// #define IRQ_PIN 32
// #define RST_PIN 14
// #define I2C_BUS 0

// ESP32 IoT RedBoard
#define IRQ_PIN 26
#define RST_PIN 27
#define I2C_BUS 0

// rp2350 thing plus
// #define IRQ_PIN 11
// #define RST_PIN 12
// #define I2C_BUS 0

// rp2350 RedBoard IoT
// #define IRQ_PIN 29
// #define RST_PIN 28
// #define I2C_BUS 0

// variable used to keep track of the number of enrolled templates on the sensor
uint16_t numberOfTemplates = 0;

// Declare our sensor object
SfeFPC2534I2C mySensor;

// flag used to indicate if the sensor has been initialized
bool isInitialized = false;

// flag to indicate we need to draw the menu
bool drawTheMenu = false;

//------------------------------------------------------------------------------------
// Simple menu methods
//------------------------------------------------------------------------------------
static void drawMenu()
{
    // clear the menu flag and "draw"/printout the menu to the terminal
    drawTheMenu = false;

    mySensor.setLED(false);

    Serial.println();
    Serial.println("----------------------------------------------------------------");
    Serial.println(" SparkFun FPC2534 Fingerprint Enrollment Example");
    Serial.println("----------------------------------------------------------------");
    Serial.println();
    Serial.print(" Current number of enrolled templates: ");
    Serial.print(numberOfTemplates);
    Serial.println(" used of 30 maximum");
    Serial.println();
    Serial.println(" Select an option (press the menu number):");
    Serial.println("\t1)  To Enroll a new fingerprint");
    Serial.println("\t2)  Erase all existing fingerprint templates");
    Serial.println("\t3)  Validate a fingerprint");
    Serial.println();

    Serial.print("> ");

    // flush the input/output buffers
    Serial.flush();
    while (Serial.available() > 0)
        Serial.read(); // flush any existing input

    // read in the menu selection - if an invalid character is entered, beep and wait for a valid entry.
    uint8_t chIn;
    while (true)
    {
        if (Serial.available() > 0)
        {
            chIn = Serial.read();
            if (chIn == '1' || chIn == '2' || chIn == '3')
            {
                Serial.println((char)chIn); // echo the character
                break;
            }
            else
                Serial.write(7); // beep - invalid character
        }
        delay(10);
    }
    Serial.println();

    // Check what item was selected?

    // Enroll a new fingerprint?
    if (chIn == '1')
    {
        // lets enroll an new figure
        Serial.println(" Starting finger enrollment - place finger and remove a finger on the sensor to enroll "
                       "a fingerprint");
        mySensor.setLED(true);
        fpc_id_type_t id = {0};
        id.type = ID_TYPE_GENERATE_NEW;
        id.id = 0;
        fpc_result_t rc = mySensor.requestEnroll(id);
        if (rc != FPC_RESULT_OK)
        {
            Serial.print("[ERROR]\tFailed to start enroll - error: ");
            Serial.println(rc);
            drawTheMenu = true;
        }
        else
            Serial.print("\t samples remaining 12..");
    }
    else if (chIn == '2')
    {
        // Delete all templates - do we have any templates?
        if (numberOfTemplates == 0)
        {
            Serial.println("[INFO]\tNo templates to delete");
            drawTheMenu = true;
        }
        else
        {
            Serial.println(" Deleting all templates on the fingerprint sensor");
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
        // Check if a fingerprint is enrolled? First, do we have any templates on the sensor?
        if (numberOfTemplates == 0)
        {
            Serial.println("[INFO]\tNo templates to validate against");
            drawTheMenu = true;
        }
        else
        {
            Serial.print(" Place a finger on the sensor for validation");

            // Send ID request to the sensor - compair to all templates
            fpc_id_type_t id = {0};
            id.type = ID_TYPE_ALL;
            id.id = 0;

            fpc_result_t rc = mySensor.requestIdentify(id, 1);
            if (rc != FPC_RESULT_OK)
            {
                Serial.print("[ERROR]\tFailed to start identity - error: ");
                Serial.println(rc);
            }
        }
    }
    else
        Serial.println("[ERROR]\tInvalid selection");
}

//------------------------------------------------------------------------------------
// Callback functions the library calls
//------------------------------------------------------------------------------------

//----------------------------------------------------------------------------
// on_error()
//
// Called if the sensor library detects/encounters an error
//
static void on_error(uint16_t error)
{
    Serial.print("[ERROR]\tSensor Error Code: ");
    Serial.println(error);

    // this could indicated the sensor communications is out of synch - a reset might be needed
    reset_sensor();
}

//----------------------------------------------------------------------------
// on_is_ready_change()
//
// Called when the device ready state changes
//
static void on_is_ready_change(bool isReady)
{
    // On startup the device isn't immediately ready. A message is sent when it is.
    // The Library will call this function when that happens

    if (isReady)
    {
        Serial.println("[STARTUP]\tFPC2534 Device is ready");

        // Request the templates on the device ...
        fpc_result_t rc = mySensor.requestListTemplates();
        if (rc != FPC_RESULT_OK)
        {
            Serial.print("[ERROR]\tFailed to get template list - error: ");
            Serial.println(rc);
        }
    }
}

//----------------------------------------------------------------------------
// on_identify()
//
// Called when the sensor sends an identify result
//
static void on_identify(bool is_match, uint16_t id)
{

    Serial.print(is_match ? "" : " NO ");
    Serial.print(" MATCH ");

    if (is_match)
    {
        Serial.print("  {Template ID: ");
        Serial.print(id);
        Serial.print("}");
    }
    Serial.println();
}
//----------------------------------------------------------------------------
// on_enroll()
//
// Called when the sensor sends an enroll result
//
static void on_enroll(uint8_t feedback, uint8_t samples_remaining)
{
    // Done?
    if (samples_remaining == 0)
    {
        Serial.println("..done!");
        delay(500); // user feedback...
        numberOfTemplates++;
    }
    else
    {
        Serial.print(samples_remaining);
        Serial.print(".");
    }
}
//----------------------------------------------------------------------------
// on_list_templates()
//
// Called when the sensor sends a list of enrolled templates
//
static void on_list_templates(uint16_t num_templates, uint16_t *template_ids)
{
    numberOfTemplates = num_templates;

    // Part of the startup sequence for this demo is to request the list of templates.
    // Now that we have the number of  templates on the sensor, start normal ops for the demo
    isInitialized = true;

    // lets draw the menu!
    drawTheMenu = true;
}

//----------------------------------------------------------------------------
// on_status()
//
// Called when the sensor sends a status message
//
static void on_status(uint16_t event, uint16_t state)
{

    // Check the system state, to determine when to draw the menu.
    //
    // The following is checked:
    //
    // 1) End of Enroll or Identify - indicated by:
    //
    //     - EVENT_FINGER_LOST event type
    //     - Sensor is in idel mode
    //
    // 2) Completetion of the Delete All Templates command
    //
    //     - EVENT_NONE event type
    //     - Sensor is in idle mode
    //     - Sensor State is STATE_APP_FW_READY
    //
    //     When this command is completed, a EVENT_NONE event is sent, and the

    if (mySensor.currentMode() == 0)
    {
        // end of enroll or identify
        if (event == EVENT_FINGER_LOST)
            drawTheMenu = true;
        // is the app ready?
        else if ((state & STATE_APP_FW_READY) == STATE_APP_FW_READY)
        {
            // completion of delete all templates
            if (event == EVENT_NONE)
                drawTheMenu = true;
            // the sensor is at idle and has been setup
            else if (event == EVENT_IDLE && isInitialized)
                drawTheMenu = true; // just in
        }
    }

    // if checking identity and we get an image ready event - the device hangs until finger up
    // This "hanging" happens when using I2C. It's worth noting this is not an issue when using UART
    //
    // Let's prompt the user to remove the finger
    //
    else if (mySensor.currentMode() == STATE_IDENTIFY && event == EVENT_IMAGE_READY)
    {
        Serial.println(" -\tUnable to perform ID check - remove finger and try again");
    }
    else if (mySensor.currentMode() == STATE_ENROLL && event == EVENT_FINGER_LOST)
    {
        // ... enroll progress  - lets write out a dot...
        Serial.print(".");
    }
}

//------------------------------------------------------------------------------------
// Callbacks
//
// Define our command callbacks structure - callback methods are assigned in setup
static sfDevFPC2534Callbacks_t cmd_cb = {0};

//------------------------------------------------------------------------------------
// reset_sensor()
//
// Simple function to toggle the reset pin of the sensor
//
void reset_sensor(void)
{
    // Reset the sensor by toggling the reset pin.
    //
    // clear out our data buffer
    mySensor.clearData();
    pinMode(RST_PIN, OUTPUT);
    digitalWrite(RST_PIN, LOW); // Set reset pin low
    delay(10);                  // Wait for 10 ms

    digitalWrite(RST_PIN, HIGH); // Set reset pin high
    delay(250);                  // Wait for sensor to initialize
}

//------------------------------------------------------------------------------------
// setup()
//
void setup()
{
    delay(2000);

    // Set up serial communication for debugging
    Serial.begin(115200); // Set baud rate to 115200
    while (!Serial)
    {
        ; // Wait for serial port to connect. Needed for native USB port only
    }
    Serial.println();
    Serial.println("----------------------------------------------------------------");
    Serial.println(" SparkFun FPC2534 Fingerprint Enrollment Example");
    Serial.println("----------------------------------------------------------------");
    Serial.println();

    // Initialize the I2C communication
    Wire.begin();

    // Reset the sensor to ensure it's in a known state - by default this also triggers the
    // sensor to send a status message
    reset_sensor();

    // Is the sensor there - on the I2C bus?
    Wire.beginTransmission(kFPC2534DefaultAddress);
    if (Wire.endTransmission() != 0)
    {
        Serial.println("[ERROR]\tTouch Sensor FPC2534 not found on I2C bus. HALT");
        while (1)
        {
            delay(1000); // Wait indefinitely if device is not found
        }
    }
    else
        Serial.println("[STARTUP]\tTouch Sensor FPC2534 found on I2C bus");

    // Initialize the sensor library
    if (!mySensor.begin(kFPC2534DefaultAddress, Wire, I2C_BUS, IRQ_PIN))
    {
        Serial.println("[ERROR]\tFPC2534 not found. Check wiring. HALT.");
        while (1)
            delay(1000);
    }
    Serial.println("[STARTUP]\tFPC2534 initialized.");

    // setup our callback functions structure
    cmd_cb.on_error = on_error;
    cmd_cb.on_status = on_status;
    cmd_cb.on_enroll = on_enroll;
    cmd_cb.on_identify = on_identify;
    cmd_cb.on_list_templates = on_list_templates;
    cmd_cb.on_is_ready_change = on_is_ready_change;

    // set the callbacks for the sensor library to call
    mySensor.setCallbacks(cmd_cb);

    // One last reset of the sensor = observation shows that this is needed after the above device ping...
    reset_sensor();

    // Ready to go!
    Serial.println("[STARTUP]\tFingerprint system initialized.");
}

//------------------------------------------------------------------------------------
void loop()
{

    // Call the library to process the next response from the sensor. The library will call our above
    // callback functions as events occur.
    fpc_result_t rc = mySensor.processNextResponse();
    if (rc != FPC_RESULT_OK && rc != FPC_PENDING_OPERATION)
    {
        Serial.print("[ERROR] Processing Error: ");
        Serial.println(rc);
        // Hmm - reset the sensor and start again?
        reset_sensor();
    }
    else if (drawTheMenu)
    {
        drawMenu();
    }

    delay(200);
}