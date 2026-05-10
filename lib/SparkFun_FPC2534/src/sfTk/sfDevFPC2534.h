/**
 * @file sfDevFPC2534.h
 * @brief header file for the SparkFun FPC2534 core implementation. The core implements all logic
 * and functionality of the FPC2534 sensor, independent of the communication protocol used.
 *
 * @copyright  (c) 2025, SparkFun Electronics Inc.
 *
 * SPDX-License-Identifier: MIT
 *
 *---------------------------------------------------------------------------------
 */

#pragma once

// from the FPC SDK
#include "fpc_api.h"

// The interface definition for communication classes
#include "sfDevFPC2534IComm.h"

// This library is dependent on Arduino framework
#include <Arduino.h>

// Define the LED pin on the FPC2534 board
const uint8_t SPARKFUN_FPC2534_LED_PIN = 1;

// The design pattern that the library implements follows the standard implementation
// pattern of the FPC SDK - response from the sensor is delivered via callback functions.
//
// Define a struct that can hold all the callback functions
// that the library can call on events from the sensor

/// @struct sfDevFPC2534Callbacks_t
/// @brief Struct holding all callback function pointers for sfDevFPC2534 events
typedef struct
{
    void (*on_error)(uint16_t error);
    void (*on_status)(uint16_t event, uint16_t state);
    void (*on_version)(char *version);
    void (*on_enroll)(uint8_t feedback, uint8_t samples_remaining);
    void (*on_identify)(bool is_match, uint16_t id);
    void (*on_list_templates)(uint16_t num_templates, uint16_t *template_ids);
    void (*on_navigation)(uint16_t gesture);
    void (*on_gpio_control)(uint8_t state);
    void (*on_system_config_get)(fpc_system_config_t *cfg);
    void (*on_bist_done)(uint16_t test_verdict);
    void (*on_data_transfer_done)(uint8_t *data, size_t size);
    void (*on_mode_change)(uint16_t new_mode);
    void (*on_finger_change)(bool present);
    void (*on_is_ready_change)(bool isReady);

} sfDevFPC2534Callbacks_t;

/// @class sfDevFPC2534
/// @brief Core class implementing FPC2534 functionality independent of communication protocol
class sfDevFPC2534
{
  public:
    sfDevFPC2534();

    fpc_result_t requestStatus(void);
    fpc_result_t requestVersion(void);
    fpc_result_t requestEnroll(fpc_id_type_t &id);
    fpc_result_t requestIdentify(fpc_id_type_t &id, uint16_t tag);
    fpc_result_t requestAbort(void);
    fpc_result_t requestListTemplates(void);
    fpc_result_t requestDeleteTemplate(fpc_id_type_t &id);
    fpc_result_t sendReset(void);
    fpc_result_t startNavigationMode(uint8_t orientation);
    fpc_result_t startBuiltInSelfTest(void);
    fpc_result_t requestSetGPIO(uint8_t pin, uint8_t mode, uint8_t state);
    fpc_result_t requestGetGPIO(uint8_t pin);
    fpc_result_t setSystemConfig(fpc_system_config_t *cfg);
    fpc_result_t requestGetSystemConfig(uint8_t type);
    fpc_result_t factoryReset(void);

    uint16_t currentMode(void) const
    {
        return _current_state & (STATE_ENROLL | STATE_IDENTIFY | STATE_NAVIGATION);
    }

    bool isFingerPresent(void) const
    {
        return _finger_present;
    }

    void setCallbacks(const sfDevFPC2534Callbacks_t &callbacks)
    {
        _callbacks = callbacks;
    }

    bool initialize(sfDevFPC2534IComm &comm)
    {
        _comm = &comm;
        return true;
    }

    bool isReady(void) const
    {
        return (_current_state & STATE_APP_FW_READY) == STATE_APP_FW_READY;
    }

    bool isDataAvailable(void) const
    {
        if (_comm == nullptr)
            return false;
        return _comm->dataAvailable();
    }

    void clearData(void)
    {
        if (_comm != nullptr)
            _comm->clearData();
    }

    fpc_result_t setLED(bool on = true);

    fpc_result_t processNextResponse(bool flushNone);
    fpc_result_t processNextResponse(void)
    {
        return processNextResponse(false);
    };

  private:
    fpc_result_t sendCommand(fpc_cmd_hdr_t &cmd, size_t size);
    fpc_result_t parseStatusCommand(fpc_cmd_hdr_t *, size_t);
    fpc_result_t parseVersionCommand(fpc_cmd_hdr_t *, size_t);
    fpc_result_t parseEnrollStatusCommand(fpc_cmd_hdr_t *, size_t);
    fpc_result_t parseIdentifyCommand(fpc_cmd_hdr_t *, size_t);
    fpc_result_t parseListTemplatesCommand(fpc_cmd_hdr_t *, size_t);
    fpc_result_t parseNavigationEventCommand(fpc_cmd_hdr_t *, size_t);
    fpc_result_t parseGPIOControlCommand(fpc_cmd_hdr_t *, size_t);
    fpc_result_t parseGetSystemConfigCommand(fpc_cmd_hdr_t *, size_t);
    fpc_result_t parseBISTCommand(fpc_cmd_hdr_t *, size_t);
    fpc_result_t parseCommand(uint8_t *frame_payload, size_t payload_size);

    bool checkForNoneEvent(uint8_t *payload, size_t size);
    fpc_result_t flushNoneEvent(void);

    sfDevFPC2534IComm *_comm = nullptr;
    sfDevFPC2534Callbacks_t _callbacks;
    uint16_t _current_state = 0;
    bool _finger_present = false;
};
