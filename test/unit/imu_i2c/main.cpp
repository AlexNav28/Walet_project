/**
 * @file    test/unit/imu_i2c/main.cpp
 * @brief   Unit test: LSM6DSO 6-DoF IMU over I2C.
 *
 * Build and flash:
 *     pio run -e test_imu_i2c -t upload -t monitor
 *
 * What it proves, in order:
 *   1. The I2C bus is alive and the part is at the address the schematic says.
 *   2. WHO_AM_I reads back 0x6C, so this is an LSM6DSO and not a floating bus
 *      that happens to ACK.
 *   3. The 12-byte block read across the gyro and accelerometer output
 *      registers returns physically sensible values.
 *
 * It is also how the theft thresholds were tuned: leave it streaming, perform
 * the motion you care about, and read the peak jerk and rotation off the "PEAK"
 * line. THEFT_ACCEL_JERK_G and THEFT_GYRO_ROTATION_DPS in
 * include/wallet_config.h came from this, with margin - walking and sitting
 * down had to stay clearly under what a snatch produced.
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#define IMU_SDA_PIN 36
#define IMU_SCL_PIN 37
#define IMU_I2C_HZ  400000
#define IMU_I2C_ADDR 0x6A

#define REG_WHO_AM_I  0x0F
#define REG_CTRL1_XL  0x10
#define REG_CTRL2_G   0x11
#define REG_OUTX_L_G  0x22

#define WHO_AM_I_LSM6DSO 0x6C

#define ACCEL_MG_PER_LSB  0.122f
#define GYRO_MDPS_PER_LSB 17.50f

static float peakJerk = 0.0f;
static float peakRotation = 0.0f;
static float lastMagnitude = 1.0f;

static bool writeRegister(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(IMU_I2C_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

static bool readRegister(uint8_t reg, uint8_t *value)
{
    Wire.beginTransmission(IMU_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0)
        return false;

    Wire.requestFrom(IMU_I2C_ADDR, 1);
    if (!Wire.available())
        return false;

    *value = Wire.read();
    return true;
}

static void scanBus(void)
{
    Serial.println("  scanning I2C bus...");
    int found = 0;
    for (uint8_t addr = 1; addr < 127; ++addr)
    {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0)
        {
            Serial.printf("    device at 0x%02X\n", addr);
            found++;
        }
    }
    Serial.printf("  %d device(s) on the bus\n", found);
}

// One 16-bit little-endian sample off the Wire FIFO, low byte first.
static int16_t readAxis(void)
{
    const uint8_t lo = (uint8_t)Wire.read();
    const uint8_t hi = (uint8_t)Wire.read();
    return (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

void setup()
{
    delay(2000);
    Serial.begin(115200);
    while (!Serial)
        delay(10);

    Serial.println();
    Serial.println("========================================");
    Serial.println("  UNIT TEST - LSM6DSO IMU over I2C");
    Serial.printf("  SDA=GPIO%d  SCL=GPIO%d  %d Hz\n", IMU_SDA_PIN, IMU_SCL_PIN, IMU_I2C_HZ);
    Serial.println("========================================");

    Wire.begin(IMU_SDA_PIN, IMU_SCL_PIN, IMU_I2C_HZ);
    delay(50);

    scanBus();

    uint8_t whoAmI = 0;
    if (!readRegister(REG_WHO_AM_I, &whoAmI))
    {
        Serial.println("  *** FAIL *** no response to WHO_AM_I.");
        Serial.println("  Check: SDA/SCL swapped? pull-ups fitted? part powered?");
        return;
    }

    Serial.printf("  WHO_AM_I = 0x%02X (expected 0x%02X)\n", whoAmI, WHO_AM_I_LSM6DSO);
    if (whoAmI != WHO_AM_I_LSM6DSO)
    {
        Serial.println("  *** FAIL *** wrong part, or the bus is reading noise.");
        return;
    }

    writeRegister(REG_CTRL1_XL, 0x40); // accel 104 Hz, +/-4 g
    delay(10);
    writeRegister(REG_CTRL2_G, 0x40); // gyro 104 Hz, +/-500 dps
    delay(50);

    Serial.println("  *** PASS *** IMU identified and configured.");
    Serial.println();
    Serial.println("  Streaming. Move the board to characterise a motion;");
    Serial.println("  PEAK values are what the theft thresholds are set from.");
    Serial.println();
}

void loop()
{
    Wire.beginTransmission(IMU_I2C_ADDR);
    Wire.write(REG_OUTX_L_G);
    if (Wire.endTransmission(false) != 0)
    {
        Serial.println("  bus error on block read");
        delay(500);
        return;
    }

    Wire.requestFrom(IMU_I2C_ADDR, 12);
    if (Wire.available() < 12)
    {
        Serial.println("  short block read");
        delay(500);
        return;
    }

    // Byte order has to be explicit: the operands of `|` are unsequenced in C++,
    // so `Wire.read() | (Wire.read() << 8)` lets the compiler pick which FIFO
    // byte is the low half. A silent swap here would calibrate the firmware's
    // theft thresholds against nonsense.
    const int16_t rawGyroX = readAxis();
    const int16_t rawGyroY = readAxis();
    const int16_t rawGyroZ = readAxis();
    const int16_t rawAccX = readAxis();
    const int16_t rawAccY = readAxis();
    const int16_t rawAccZ = readAxis();

    const float ax = rawAccX * ACCEL_MG_PER_LSB / 1000.0f;
    const float ay = rawAccY * ACCEL_MG_PER_LSB / 1000.0f;
    const float az = rawAccZ * ACCEL_MG_PER_LSB / 1000.0f;
    const float gx = rawGyroX * GYRO_MDPS_PER_LSB / 1000.0f;
    const float gy = rawGyroY * GYRO_MDPS_PER_LSB / 1000.0f;
    const float gz = rawGyroZ * GYRO_MDPS_PER_LSB / 1000.0f;

    const float magnitude = sqrtf(ax * ax + ay * ay + az * az);
    const float jerk = fabsf(magnitude - lastMagnitude);
    const float rotation = sqrtf(gx * gx + gy * gy + gz * gz);
    lastMagnitude = magnitude;

    if (jerk > peakJerk)
        peakJerk = jerk;
    if (rotation > peakRotation)
        peakRotation = rotation;

    static uint32_t sample = 0;
    if ((sample++ % 50) == 0)
    {
        // At rest |a| should sit at 1.00 g with the board flat on the bench.
        Serial.printf("  |a| %.2f g  jerk %.3f g  rot %.1f dps   PEAK jerk %.3f g  rot %.1f dps\n",
                      magnitude, jerk, rotation, peakJerk, peakRotation);
    }

    delay(10);
}
