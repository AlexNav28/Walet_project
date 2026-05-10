/**
 * @file uart_loopback_test.cpp
 * @brief UART Loopback Test - Find working Serial1 pins
 *
 * Connect SENSOR_TX_PIN to SENSOR_RX_PIN with a jumper wire (no sensor).
 * If the test prints "PASS", those pins work for Serial1.
 * Change the pin numbers until you find a working pair.
 */

#include <Arduino.h>

// ============================================================================
// CHANGE THESE PINS until the test passes
// ============================================================================
#define SENSOR_TX_PIN  1   // Try: 1, 10, 12, 13, 5, 6
#define SENSOR_RX_PIN  2   // Try: 2, 9, 11, 14, 7, 8

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
