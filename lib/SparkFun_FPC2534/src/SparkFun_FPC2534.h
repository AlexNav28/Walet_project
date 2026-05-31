/**
 * @file SparkFun_FPC2534.h
 * @brief Arduino library header for the SparkFun FPC2534 Fingerprint Sensor
 *
 * This provides the Arduino-friendly wrapper class for UART communication.
 * The actual logic lives in sfDevFPC2534 (core) and sfDevFPC2534UART (transport).
 *
 * @copyright Copyright (c) 2025 SparkFun Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sfTk/sfDevFPC2534.h"
#include "sfTk/sfDevFPC2534UART.h"
#include <Arduino.h>

#define SFE_FPC2534_I2C_ADDRESS kFPC2534DefaultAddress
//--------------------------------------------------------------------------------------------
// UART/Serial version of the FPC2534 class
//
class SfeFPC2534UART : public sfDevFPC2534
{
  public:
    SfeFPC2534UART() {}

    bool begin(HardwareSerial &theUART)
    {
        if (!_commUART.initialize(theUART))
            return false;

        return sfDevFPC2534::initialize(_commUART);
    }

  private:
    sfDevFPC2534UART _commUART;
};
