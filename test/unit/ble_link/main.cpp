/**
 * @file    test/unit/ble_link/main.cpp
 * @brief   Unit test: BLE GATT service, notifications, and the command channel.
 *
 * Build and flash:
 *     pio run -e test_ble_link -t upload -t monitor
 *
 * Stands the wallet's GATT service up on its own, with no sensors attached, so
 * the phone side can be developed against a device whose behaviour is
 * predictable. The UUIDs and the status and command byte encodings are
 * identical to the firmware.
 *
 * What it proves:
 *   1. The ESP32-S3 advertises and a central can connect and subscribe.
 *   2. Notifications on the STATUS characteristic reach the phone.
 *   3. Writes to the COMMAND characteristic arrive intact.
 *   4. Disconnects are detected - the proximity signal the firmware uses to
 *      decide the wallet and its owner have separated.
 *
 * Procedure with nRF Connect (or the companion app):
 *   - connect, then subscribe to the STATUS characteristic
 *   - watch the counter notification arrive every two seconds
 *   - write 0x01 / 0x02 / 0x03 / 0x04 to COMMAND and check the monitor decodes it
 *   - walk out of range and confirm the disconnect is logged
 */

#include <Arduino.h>

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#define BLE_DEVICE_NAME "SmartWallet"

#define SERVICE_UUID      "4FAFC201-1FB5-459E-8FCC-C5C9C331914B"
#define STATUS_CHAR_UUID  "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define COMMAND_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"

static BLECharacteristic *statusChar = nullptr;
static volatile bool linked = false;

static const char *commandName(uint8_t command)
{
    switch (command)
    {
    case 0x01:
        return "ENROLL_START";
    case 0x02:
        return "DELETE_LAST";
    case 0x03:
        return "DELETE_ALL";
    case 0x04:
        return "LIST_TEMPLATES";
    default:
        return "unknown";
    }
}

class ServerCallbacks : public BLEServerCallbacks
{
    void onConnect(BLEServer *server) override
    {
        (void)server;
        linked = true;
        Serial.println("  central connected");
    }

    void onDisconnect(BLEServer *server) override
    {
        (void)server;
        linked = false;
        Serial.println("  central disconnected - this is the proximity signal");
        BLEDevice::getAdvertising()->start();
    }
};

class CommandCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *characteristic) override
    {
        const std::string value = characteristic->getValue();
        if (value.empty())
        {
            Serial.println("  empty write ignored");
            return;
        }

        const uint8_t command = static_cast<uint8_t>(value[0]);
        Serial.printf("  command 0x%02X (%s), %u byte(s)\n", command, commandName(command),
                      (unsigned)value.size());
    }
};

void setup()
{
    delay(2000);
    Serial.begin(115200);
    while (!Serial)
        delay(10);

    Serial.println();
    Serial.println("========================================");
    Serial.println("  UNIT TEST - BLE GATT link");
    Serial.printf("  advertising as \"%s\"\n", BLE_DEVICE_NAME);
    Serial.println("========================================");

    BLEDevice::init(BLE_DEVICE_NAME);

    BLEServer *server = BLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());

    BLEService *service = server->createService(SERVICE_UUID);

    statusChar = service->createCharacteristic(
        STATUS_CHAR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    statusChar->addDescriptor(new BLE2902());

    BLECharacteristic *commandChar =
        service->createCharacteristic(COMMAND_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
    commandChar->setCallbacks(new CommandCallbacks());

    service->start();
    BLEDevice::getAdvertising()->addServiceUUID(SERVICE_UUID);
    BLEDevice::getAdvertising()->start();

    Serial.println("  *** PASS *** stack up and advertising.");
    Serial.println("  Connect with nRF Connect and subscribe to STATUS.");
    Serial.println();
}

void loop()
{
    static uint8_t counter = 0;

    if (linked)
    {
        statusChar->setValue(&counter, 1);
        statusChar->notify();
        Serial.printf("  notified 0x%02X\n", counter);
        counter++;
    }
    else
    {
        Serial.println("  waiting for a central...");
    }

    delay(2000);
}
