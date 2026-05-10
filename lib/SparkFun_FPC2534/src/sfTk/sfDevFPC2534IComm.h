/**
 * @file sfDevFPC2534IComm.h
 * @brief Abstract communication interface for the FPC2534 sensor.
 *
 * This defines the base class that all communication implementations (UART, I2C, SPI)
 * must inherit from. It provides a uniform API for the core sfDevFPC2534 class to
 * send/receive data regardless of the underlying transport.
 *
 * @copyright Copyright (c) 2025, SparkFun Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "fpc_api.h"

/**
 * @class sfDevFPC2534IComm
 * @brief Abstract interface for FPC2534 communication transports.
 *
 * Any transport (UART, I2C, SPI) implements this interface so the core
 * library logic remains protocol-agnostic.
 */
class sfDevFPC2534IComm
{
  public:
    virtual ~sfDevFPC2534IComm() {}

    // Check if data is available to read from the sensor
    virtual bool dataAvailable(void) = 0;

    // Discard any buffered incoming data
    virtual void clearData(void) = 0;

    // Write raw bytes to the sensor, returns FPC_RESULT_OK on success
    virtual uint16_t write(const uint8_t *data, size_t len) = 0;

    // Read raw bytes from the sensor, returns FPC_RESULT_OK on success
    virtual uint16_t read(uint8_t *data, size_t len) = 0;

    // Called before a sequence of write() calls (framing start)
    virtual void beginWrite(void) {}

    // Called after a sequence of write() calls (framing end / flush)
    virtual void endWrite(void) {}

    // Called before a sequence of read() calls
    virtual void beginRead(void) {}

    // Called after a sequence of read() calls
    virtual void endRead(void) {}
};
