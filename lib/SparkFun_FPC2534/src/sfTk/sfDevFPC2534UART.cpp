/*
 *---------------------------------------------------------------------------------
 *
 * Copyright (c) 2025, SparkFun Electronics Inc.
 *
 * SPDX-License-Identifier: MIT
 *
 *---------------------------------------------------------------------------------
 */

#include "sfDevFPC2534UART.h"

sfDevFPC2534UART::sfDevFPC2534UART() : _theUART{nullptr}
{
}

//--------------------------------------------------------------------------------------------
bool sfDevFPC2534UART::initialize(HardwareSerial &theUART)
{
    _theUART = &theUART;
    return true;
}

//--------------------------------------------------------------------------------------------
bool sfDevFPC2534UART::dataAvailable(void)
{
    if (_theUART == nullptr)
        return false;

    return _theUART->available() > 0;
}

//--------------------------------------------------------------------------------------------
void sfDevFPC2534UART::clearData()
{
    if (_theUART == nullptr)
        return;

    while (_theUART->available() > 0)
        _theUART->read();
}

//--------------------------------------------------------------------------------------------
uint16_t sfDevFPC2534UART::write(const uint8_t *data, size_t len)
{
    if (_theUART == nullptr)
        return FPC_RESULT_IO_RUNTIME_FAILURE;

    size_t nWritten = _theUART->write(data, len);
    return nWritten == len ? FPC_RESULT_OK : FPC_RESULT_FAILURE;
}

//--------------------------------------------------------------------------------------------
uint16_t sfDevFPC2534UART::read(uint8_t *data, size_t len)
{
    if (_theUART == nullptr)
        return FPC_RESULT_IO_RUNTIME_FAILURE;

    size_t readBytes = _theUART->available();

    if (readBytes == 0 || readBytes < len)
        return FPC_RESULT_IO_NO_DATA;

    readBytes = _theUART->readBytes(data, len);

    if (readBytes == 0 && len > 0)
        return FPC_RESULT_IO_NO_DATA;

    return FPC_RESULT_OK;
}
