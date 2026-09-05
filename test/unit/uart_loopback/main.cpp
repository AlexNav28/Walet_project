/**
 * @file    test/unit/uart_loopback/main.cpp
 * @brief   Unit test: prove a Serial1 pin pair works before blaming the sensor.
 *
 * Build and flash:
 *     pio run -e test_uart_loopback -t upload -t monitor
 *
 * When the fingerprint sensor returned nothing, the suspects were the sensor,
 * the ribbon, the ZIF connector and the ESP32 UART pin mapping. This sketch
 * eliminates the last one: jumper TX to RX, send five bytes, check they come
 * back. A PASS means the ESP32 side is fine and the fault is downstream, which
 * is how the unpowered ZIF connector was found.
 *
 * Wiring: one jumper from SENSOR_TX_PIN to SENSOR_RX_PIN, no sensor connected.
 * Change the pin defines and reflash to qualify another pair.
 */

#include <Arduino.h>

// ============================================================================
// The pins under test. Defaults are the ZIF connector's UART pins.
// ============================================================================
#define SENSOR_TX_PIN 17 // the board's fingerprint TX; change to qualify another pair
#define SENSOR_RX_PIN 18 // the board's fingerprint RX

void setup()
{
    delay(2000);
    Serial.begin(115200);
    while (!Serial) delay(10);

    Serial.println();
    Serial.println("================================");
    Serial.println("  UART Loopback Test");
    Serial.printf("  TX pin: GPIO%d\n", SENSOR_TX_PIN);
    Serial.printf("  RX pin: GPIO%d\n", SENSOR_RX_PIN);
    Serial.println("================================");
    Serial.println();
    Serial.println("  Connect a jumper wire from TX pin to RX pin.");
    Serial.println("  Testing in 3 seconds...");
    Serial.println();
    delay(3000);

    // Start Serial1 on the test pins
    Serial1.setRxBufferSize(256);
    Serial1.begin(921600, SERIAL_8N1, SENSOR_RX_PIN, SENSOR_TX_PIN);
    delay(100);

    // Send 5 test bytes
    uint8_t testData[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    Serial1.write(testData, 5);
    delay(50);

    // Check what came back
    int available = Serial1.available();
    Serial.printf("  Bytes sent: 5\n");
    Serial.printf("  Bytes received: %d\n", available);

    if (available == 5)
    {
        bool match = true;
        for (int i = 0; i < 5; i++)
        {
            uint8_t b = Serial1.read();
            if (b != testData[i])
            {
                Serial.printf("  MISMATCH at byte %d: sent 0x%02X, got 0x%02X\n", i, testData[i], b);
                match = false;
            }
        }

        if (match)
        {
            Serial.println();
            Serial.println("  *** PASS *** These pins work!");
            Serial.printf("  Use: TX=GPIO%d, RX=GPIO%d\n", SENSOR_TX_PIN, SENSOR_RX_PIN);
        }
        else
        {
            Serial.println("\n  FAIL - data corrupted (baud rate issue?)");
        }
    }
    else if (available == 0)
    {
        Serial.println("\n  FAIL - no data received.");
        Serial.println("  Check: jumper wire connected TX to RX?");
        Serial.println("  Or try different GPIO pins.");
    }
    else
    {
        Serial.printf("\n  FAIL - expected 5 bytes, got %d\n", available);
    }
}

void loop()
{
    delay(1000);
}
