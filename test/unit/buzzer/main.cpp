/**
 * @file    test/unit/buzzer/main.cpp
 * @brief   Unit test: piezo buzzer driven through its N-MOS low-side switch.
 *
 * Build and flash:
 *     pio run -e test_buzzer -t upload -t monitor
 *
 * HISTORICAL. This passes only on a board that still has the buzzer's N-MOS
 * fitted. On the reworked hardware that N-MOS was removed so its GPIO could
 * drive the servo latch, so the shipped firmware has no audible alarm
 * (WALLET_HAS_BUZZER is 0 in include/wallet_config.h). Kept because it is the
 * test that caught the transistor being wired wrong.
 *
 * The bug it found: on rev A the N-MOS gate and source were swapped in the
 * layout, so the device never switched cleanly - the buzzer stayed silent or
 * hummed weakly regardless of the GPIO state. Fixed on the bench with a
 * replacement part, legs bent to the correct pads.
 *
 * Expected behaviour on good hardware:
 *   - "steady" gives a continuous tone
 *   - "pulse" gives ten clean 150 ms beeps
 *   - the buzzer is silent between beeps, not humming
 */

#include <Arduino.h>

#define BUZZER_PIN 1 // the gate net the servo took over during rework

#define BEEP_COUNT  10
#define BEEP_ON_MS  150
#define BEEP_OFF_MS 100

#define TONE_HZ       2700 // the piezo element's resonant peak
#define TONE_CHANNEL  4    // an LEDC channel the servo test does not claim

void setup()
{
    delay(2000);
    Serial.begin(115200);
    while (!Serial)
        delay(10);

    Serial.println();
    Serial.println("========================================");
    Serial.println("  UNIT TEST - buzzer (historical)");
    Serial.printf("  N-MOS gate on GPIO%d\n", BUZZER_PIN);
    Serial.println("========================================");
    Serial.println("  NOTE: this N-MOS was removed during rework so the servo");
    Serial.println("  could use this GPIO. Expect silence on current hardware.");
    Serial.println();
    Serial.println("  Commands: p pulse train | t steady tone | o off");
    Serial.println();

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    Serial.println("  Running the pulse train once...");
    for (int i = 1; i <= BEEP_COUNT; ++i)
    {
        Serial.printf("    beep %d/%d\n", i, BEEP_COUNT);
        digitalWrite(BUZZER_PIN, HIGH);
        delay(BEEP_ON_MS);
        digitalWrite(BUZZER_PIN, LOW);
        delay(BEEP_OFF_MS);
    }
    Serial.println("  Done. Audible and cleanly gated = PASS.");
    Serial.println();
}

void loop()
{
    if (!Serial.available())
    {
        delay(20);
        return;
    }

    String line = Serial.readStringUntil('\n');
    line.trim();

    if (line == "p")
    {
        for (int i = 1; i <= BEEP_COUNT; ++i)
        {
            digitalWrite(BUZZER_PIN, HIGH);
            delay(BEEP_ON_MS);
            digitalWrite(BUZZER_PIN, LOW);
            delay(BEEP_OFF_MS);
        }
        Serial.println("  pulse train done");
    }
    else if (line == "t")
    {
        // A square wave at the element's resonance is much louder than a
        // straight DC level, which matters for an alarm inside a pocket.
        ledcSetup(TONE_CHANNEL, TONE_HZ, 8);
        ledcAttachPin(BUZZER_PIN, TONE_CHANNEL);
        ledcWrite(TONE_CHANNEL, 128); // 50% duty
        Serial.printf("  steady tone at %d Hz - send 'o' to stop\n", TONE_HZ);
    }
    else if (line == "o")
    {
        ledcWrite(TONE_CHANNEL, 0);
        ledcDetachPin(BUZZER_PIN);
        pinMode(BUZZER_PIN, OUTPUT);
        digitalWrite(BUZZER_PIN, LOW);
        Serial.println("  off");
    }
}
