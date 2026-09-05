/**
 * @file    test/unit/servo_lock/main.cpp
 * @brief   Unit test: servo latch on the repurposed buzzer GPIO.
 *
 * Build and flash:
 *     pio run -e test_servo_lock -t upload -t monitor
 *
 * On rev A the motor control net was never routed to a GPIO, so the latch could
 * not be driven. The fix was to desolder the buzzer's N-MOS and take over its
 * GPIO. This sketch confirmed the rework before any of it went into the
 * firmware.
 *
 * It is also how the end-stop angles were found: type an angle and the servo
 * goes there. The angles at which the latch is fully engaged and fully clear
 * became SERVO_LOCKED_ANGLE and SERVO_UNLOCKED_ANGLE in
 * include/wallet_config.h.
 *
 * Commands (type a line, then Enter):
 *     <number>   drive to that angle, 0..180
 *     s          sweep locked -> unlocked -> locked
 *     l          go to the locked angle
 *     u          go to the unlocked angle
 */

#include <Arduino.h>
#include <ESP32Servo.h>

#define SERVO_PIN 1 // was the buzzer gate; see docs/03-board-bringup-and-debugging.md

#define LOCKED_ANGLE   180
#define UNLOCKED_ANGLE 120
#define PWM_HZ         50
#define MIN_PULSE_US   500
#define MAX_PULSE_US   2400
#define TRAVEL_MS      800

static Servo servo;

static void driveTo(int angle)
{
    angle = constrain(angle, 0, 180);
    Serial.printf("  -> %d deg\n", angle);

    servo.setPeriodHertz(PWM_HZ);
    servo.attach(SERVO_PIN, MIN_PULSE_US, MAX_PULSE_US);
    servo.write(angle);
    delay(TRAVEL_MS);

    // Detach between moves. Left attached, the servo hunts around its setpoint
    // and keeps drawing current - unaffordable on a wallet-sized cell.
    servo.detach();
}

void setup()
{
    delay(2000);
    Serial.begin(115200);
    while (!Serial)
        delay(10);

    Serial.println();
    Serial.println("========================================");
    Serial.println("  UNIT TEST - servo latch");
    Serial.printf("  signal on GPIO%d, %d Hz, %d-%d us\n", SERVO_PIN, PWM_HZ, MIN_PULSE_US,
                  MAX_PULSE_US);
    Serial.println("========================================");
    Serial.println("  Commands: <angle> | s sweep | l locked | u unlocked");
    Serial.println();

    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    ESP32PWM::allocateTimer(2);
    ESP32PWM::allocateTimer(3);

    Serial.println("  Homing to the locked end stop.");
    driveTo(LOCKED_ANGLE);
    Serial.println("  *** PASS *** if the latch moved. If it did not, check the");
    Serial.println("  bodge wire from the servo signal to this GPIO first.");
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
    if (line.isEmpty())
        return;

    if (line == "s")
    {
        Serial.println("  sweep");
        driveTo(LOCKED_ANGLE);
        driveTo(UNLOCKED_ANGLE);
        driveTo(LOCKED_ANGLE);
    }
    else if (line == "l")
    {
        driveTo(LOCKED_ANGLE);
    }
    else if (line == "u")
    {
        driveTo(UNLOCKED_ANGLE);
    }
    else
    {
        driveTo(line.toInt());
    }
}
