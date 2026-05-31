/**
 * @file sfDevFPC2534.cpp
 * @brief Implementation of the sfDevFPC2534 device class. This contains all core logic of the library
 *
 * @copyright Copyright (c) 2025, SparkFun Electronics Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sfDevFPC2534.h"

//--------------------------------------------------------------------------------------------
sfDevFPC2534::sfDevFPC2534() : _comm{nullptr}, _callbacks{0}, _current_state{0}, _finger_present{false}
{
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::sendCommand(fpc_cmd_hdr_t &cmd, size_t size)
{
    if (_comm == nullptr)
        return FPC_RESULT_WRONG_STATE;

    fpc_frame_hdr_t frameHeader = {0};
    frameHeader.version = FPC_FRAME_PROTOCOL_VERSION;
    frameHeader.type = FPC_FRAME_TYPE_CMD_REQUEST;
    frameHeader.flags = FPC_FRAME_FLAG_SENDER_HOST;
    frameHeader.payload_size = (uint16_t)size;

    _comm->beginWrite();
    fpc_result_t rc = _comm->write((uint8_t *)&frameHeader, sizeof(fpc_frame_hdr_t));

    if (rc == FPC_RESULT_OK)
        rc = _comm->write((uint8_t *)&cmd, size);

    _comm->endWrite();
    return rc;
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::requestStatus(void)
{
    fpc_cmd_hdr_t cmd = {.cmd_id = CMD_STATUS, .type = FPC_FRAME_TYPE_CMD_REQUEST};
    return sendCommand(cmd, sizeof(fpc_cmd_hdr_t));
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::requestVersion(void)
{
    fpc_cmd_hdr_t cmd = {.cmd_id = CMD_VERSION, .type = FPC_FRAME_TYPE_CMD_REQUEST};
    return sendCommand(cmd, sizeof(fpc_cmd_hdr_t));
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::requestEnroll(fpc_id_type_t &id)
{
    if (id.type != ID_TYPE_SPECIFIED && id.type != ID_TYPE_GENERATE_NEW)
        return FPC_RESULT_INVALID_PARAM;

    fpc_cmd_enroll_request_t cmd = {.cmd = {.cmd_id = CMD_ENROLL, .type = FPC_FRAME_TYPE_CMD_REQUEST}, .tpl_id = id};
    return sendCommand((fpc_cmd_hdr_t &)cmd, sizeof(fpc_cmd_enroll_request_t));
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::requestIdentify(fpc_id_type_t &id, uint16_t tag)
{
    if (id.type != ID_TYPE_SPECIFIED && id.type != ID_TYPE_ALL)
        return FPC_RESULT_INVALID_PARAM;

    fpc_cmd_identify_request_t cmd = {
        .cmd = {.cmd_id = CMD_IDENTIFY, .type = FPC_FRAME_TYPE_CMD_REQUEST}, .tpl_id = id, .tag = tag};
    return sendCommand((fpc_cmd_hdr_t &)cmd, sizeof(fpc_cmd_identify_request_t));
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::requestAbort(void)
{
    fpc_cmd_hdr_t cmd = {.cmd_id = CMD_ABORT, .type = FPC_FRAME_TYPE_CMD_REQUEST};

    fpc_result_t rc = sendCommand(cmd, sizeof(fpc_cmd_hdr_t));
    if (rc == FPC_RESULT_OK)
    {
        delay(20);
        flushNoneEvent();
    }
    return rc;
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::requestListTemplates(void)
{
    fpc_cmd_hdr_t cmd = {.cmd_id = CMD_LIST_TEMPLATES, .type = FPC_FRAME_TYPE_CMD_REQUEST};
    return sendCommand(cmd, sizeof(fpc_cmd_hdr_t));
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::requestDeleteTemplate(fpc_id_type_t &id)
{
    if (id.type != ID_TYPE_SPECIFIED && id.type != ID_TYPE_ALL)
        return FPC_RESULT_INVALID_PARAM;

    fpc_cmd_enroll_request_t cmd = {.cmd = {.cmd_id = CMD_DELETE_TEMPLATE, .type = FPC_FRAME_TYPE_CMD_REQUEST},
                                    .tpl_id = id};
    return sendCommand((fpc_cmd_hdr_t &)cmd, sizeof(fpc_cmd_enroll_request_t));
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::sendReset(void)
{
    fpc_cmd_hdr_t cmd = {.cmd_id = CMD_RESET, .type = FPC_FRAME_TYPE_CMD_REQUEST};
    return sendCommand(cmd, sizeof(fpc_cmd_hdr_t));
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::startNavigationMode(uint8_t orientation)
{
    if (orientation > 3)
        return FPC_RESULT_INVALID_PARAM;

    fpc_cmd_navigation_request_t cmd = {.cmd = {.cmd_id = CMD_NAVIGATION, .type = FPC_FRAME_TYPE_CMD_REQUEST},
                                        .config = orientation};
    return sendCommand((fpc_cmd_hdr_t &)cmd, sizeof(fpc_cmd_navigation_request_t));
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::startBuiltInSelfTest(void)
{
    fpc_cmd_hdr_t cmd = {.cmd_id = CMD_BIST, .type = FPC_FRAME_TYPE_CMD_REQUEST};
    return sendCommand(cmd, sizeof(fpc_cmd_hdr_t));
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::requestSetGPIO(uint8_t pin, uint8_t mode, uint8_t state)
{
    if (mode > GPIO_CONTROL_MODE_INPUT_PULL_DOWN || state > GPIO_CONTROL_STATE_SET)
        return FPC_RESULT_INVALID_PARAM;

    fpc_cmd_pinctrl_gpio_request_t cmd = {.cmd = {.cmd_id = CMD_GPIO_CONTROL, .type = FPC_FRAME_TYPE_CMD_REQUEST},
                                          .sub_cmd = GPIO_CONTROL_SUB_CMD_SET,
                                          .pin = pin,
                                          .mode = mode,
                                          .state = state};
    return sendCommand((fpc_cmd_hdr_t &)cmd, sizeof(fpc_cmd_pinctrl_gpio_request_t));
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::requestGetGPIO(uint8_t pin)
{
    fpc_cmd_pinctrl_gpio_request_t cmd = {.cmd = {.cmd_id = CMD_GPIO_CONTROL, .type = FPC_FRAME_TYPE_CMD_REQUEST},
                                          .sub_cmd = GPIO_CONTROL_SUB_CMD_GET,
                                          .pin = pin,
                                          .mode = 0,
                                          .state = 0};
    return sendCommand((fpc_cmd_hdr_t &)cmd, sizeof(fpc_cmd_pinctrl_gpio_request_t));
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::setSystemConfig(fpc_system_config_t *cfg)
{
    if (cfg == nullptr)
        return FPC_RESULT_INVALID_PARAM;

    fpc_cmd_set_config_request_t cmd = {.cmd = {.cmd_id = CMD_SET_SYSTEM_CONFIG, .type = FPC_FRAME_TYPE_CMD_REQUEST},
                                        .cfg = *cfg};
    return sendCommand((fpc_cmd_hdr_t &)cmd, sizeof(fpc_cmd_set_config_request_t));
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::requestGetSystemConfig(uint8_t type)
{
    if (type > FPC_SYS_CFG_TYPE_CUSTOM)
        return FPC_RESULT_INVALID_PARAM;

    fpc_cmd_get_config_request_t cmd = {.cmd = {.cmd_id = CMD_GET_SYSTEM_CONFIG, .type = FPC_FRAME_TYPE_CMD_REQUEST},
                                        .config_type = type};
    return sendCommand((fpc_cmd_hdr_t &)cmd, sizeof(fpc_cmd_get_config_request_t));
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::factoryReset(void)
{
    fpc_cmd_hdr_t cmd = {.cmd_id = CMD_FACTORY_RESET, .type = FPC_FRAME_TYPE_CMD_REQUEST};
    return sendCommand(cmd, sizeof(fpc_cmd_hdr_t));
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::parseStatusCommand(fpc_cmd_hdr_t *cmd_hdr, size_t size)
{
    if (size != sizeof(fpc_cmd_status_response_t))
        return FPC_RESULT_INVALID_PARAM;

    fpc_cmd_status_response_t *status = (fpc_cmd_status_response_t *)cmd_hdr;

    if (status->app_fail_code != 0)
    {
        if (_callbacks.on_error)
            _callbacks.on_error(status->app_fail_code);
        return FPC_RESULT_OK;
    }

    uint16_t prev_state = _current_state;

    bool prev_finger_present = _finger_present;
    if (status->event == EVENT_FINGER_DETECT)
        _finger_present = true;
    else if (status->event == EVENT_FINGER_LOST)
        _finger_present = false;

    _current_state = status->state;

    if (currentMode() != (prev_state & (STATE_ENROLL | STATE_IDENTIFY | STATE_NAVIGATION)) && _callbacks.on_mode_change)
        _callbacks.on_mode_change(currentMode());

    if (isFingerPresent() != prev_finger_present)
    {
        if (_callbacks.on_finger_change)
            _callbacks.on_finger_change(isFingerPresent());
    }

    if (isReady() != ((prev_state & STATE_APP_FW_READY) == STATE_APP_FW_READY))
    {
        if (_callbacks.on_is_ready_change)
            _callbacks.on_is_ready_change(isReady());
    }

    if (_callbacks.on_status)
        _callbacks.on_status(status->event, status->state);

    return FPC_RESULT_OK;
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::parseVersionCommand(fpc_cmd_hdr_t *cmd_hdr, size_t size)
{
    fpc_cmd_version_response_t *ver = (fpc_cmd_version_response_t *)cmd_hdr;

    if (size != sizeof(fpc_cmd_version_response_t) + ver->version_str_len)
        return FPC_RESULT_INVALID_PARAM;

    if (_callbacks.on_version)
        _callbacks.on_version(ver->version_str);

    return FPC_RESULT_OK;
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::parseEnrollStatusCommand(fpc_cmd_hdr_t *cmd_hdr, size_t size)
{
    if (size != sizeof(fpc_cmd_enroll_status_response_t))
        return FPC_RESULT_INVALID_PARAM;

    fpc_cmd_enroll_status_response_t *status = (fpc_cmd_enroll_status_response_t *)cmd_hdr;

    if (_callbacks.on_enroll)
        _callbacks.on_enroll(status->feedback, status->samples_remaining);

    return FPC_RESULT_OK;
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::parseIdentifyCommand(fpc_cmd_hdr_t *cmd_hdr, size_t size)
{
    if (size != sizeof(fpc_cmd_identify_status_response_t))
        return FPC_RESULT_INVALID_PARAM;

    fpc_cmd_identify_status_response_t *id_res = (fpc_cmd_identify_status_response_t *)cmd_hdr;

    if (_callbacks.on_identify)
        _callbacks.on_identify(id_res->match == IDENTIFY_RESULT_MATCH, id_res->tpl_id.id);

    return FPC_RESULT_OK;
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::parseListTemplatesCommand(fpc_cmd_hdr_t *cmd_hdr, size_t size)
{
    fpc_cmd_template_info_response_t *list = (fpc_cmd_template_info_response_t *)cmd_hdr;

    if (size != sizeof(fpc_cmd_template_info_response_t) + (sizeof(uint16_t) * list->number_of_templates))
        return FPC_RESULT_INVALID_PARAM;

    if (_callbacks.on_list_templates)
        _callbacks.on_list_templates(list->number_of_templates, list->template_id_list);

    return FPC_RESULT_OK;
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::parseNavigationEventCommand(fpc_cmd_hdr_t *cmd_hdr, size_t size)
{
    if (size != sizeof(fpc_cmd_navigation_status_event_t))
        return FPC_RESULT_INVALID_PARAM;

    fpc_cmd_navigation_status_event_t *cmd_nav = (fpc_cmd_navigation_status_event_t *)cmd_hdr;

    if (_callbacks.on_navigation)
        _callbacks.on_navigation(cmd_nav->gesture);

    return FPC_RESULT_OK;
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::parseGPIOControlCommand(fpc_cmd_hdr_t *cmd_hdr, size_t size)
{
    if (size != sizeof(fpc_cmd_pinctrl_gpio_response_t))
        return FPC_RESULT_INVALID_PARAM;

    fpc_cmd_pinctrl_gpio_response_t *cmd_rsp = (fpc_cmd_pinctrl_gpio_response_t *)cmd_hdr;

    if (_callbacks.on_gpio_control)
        _callbacks.on_gpio_control(cmd_rsp->state);

    return FPC_RESULT_OK;
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::parseGetSystemConfigCommand(fpc_cmd_hdr_t *cmd_hdr, size_t size)
{
    if (size < sizeof(fpc_cmd_get_config_response_t))
        return FPC_RESULT_INVALID_PARAM;

    fpc_cmd_get_config_response_t *cmd_cfg = (fpc_cmd_get_config_response_t *)cmd_hdr;

    if (_callbacks.on_system_config_get)
        _callbacks.on_system_config_get(&cmd_cfg->cfg);

    return FPC_RESULT_OK;
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::parseBISTCommand(fpc_cmd_hdr_t *cmd_hdr, size_t size)
{
    if (size < sizeof(fpc_cmd_bist_response_t))
        return FPC_RESULT_INVALID_PARAM;

    fpc_cmd_bist_response_t *cmd_rsp = (fpc_cmd_bist_response_t *)cmd_hdr;

    if (_callbacks.on_bist_done)
        _callbacks.on_bist_done(cmd_rsp->test_verdict);

    return FPC_RESULT_OK;
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::parseCommand(uint8_t *payload, size_t size)
{
    if (payload == nullptr || size == 0)
        return FPC_RESULT_INVALID_PARAM;

    fpc_cmd_hdr_t *cmdHeader = (fpc_cmd_hdr_t *)payload;

    if (cmdHeader->type != FPC_FRAME_TYPE_CMD_EVENT && cmdHeader->type != FPC_FRAME_TYPE_CMD_RESPONSE)
        return FPC_RESULT_INVALID_PARAM;

    switch (cmdHeader->cmd_id)
    {
    case CMD_STATUS:
        return parseStatusCommand(cmdHeader, size);
    case CMD_VERSION:
        return parseVersionCommand(cmdHeader, size);
    case CMD_ENROLL:
        return parseEnrollStatusCommand(cmdHeader, size);
    case CMD_IDENTIFY:
        return parseIdentifyCommand(cmdHeader, size);
    case CMD_LIST_TEMPLATES:
        return parseListTemplatesCommand(cmdHeader, size);
    case CMD_NAVIGATION:
        return parseNavigationEventCommand(cmdHeader, size);
    case CMD_GPIO_CONTROL:
        return parseGPIOControlCommand(cmdHeader, size);
    case CMD_GET_SYSTEM_CONFIG:
        return parseGetSystemConfigCommand(cmdHeader, size);
    case CMD_BIST:
        return parseBISTCommand(cmdHeader, size);
    default:
        return FPC_RESULT_INVALID_PARAM;
    }

    return FPC_RESULT_OK;
}

//--------------------------------------------------------------------------------------------
bool sfDevFPC2534::checkForNoneEvent(uint8_t *payload, size_t size)
{
    if (payload == nullptr || size == 0)
        return false;

    fpc_cmd_hdr_t *cmdHeader = (fpc_cmd_hdr_t *)payload;

    if (cmdHeader->type != FPC_FRAME_TYPE_CMD_EVENT && cmdHeader->type != FPC_FRAME_TYPE_CMD_RESPONSE)
        return false;

    if (cmdHeader->cmd_id != CMD_STATUS)
        return false;

    if (size != sizeof(fpc_cmd_status_response_t))
        return false;

    fpc_cmd_status_response_t *status = (fpc_cmd_status_response_t *)cmdHeader;
    return status->event == EVENT_NONE;
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::processNextResponse(bool flushNone)
{
    if (_comm == nullptr)
        return FPC_RESULT_WRONG_STATE;

    if (!_comm->dataAvailable())
        return FPC_RESULT_OK;

    // --- NEW STRAT: Peek at the first byte before pulling a full header block ---
    // A valid app frame MUST start with 0x04 (Little-endian 0x0004 version parameter)
    // If the next byte in the stream isn't 0x04, it is guaranteed to be bootloader text or noise.
    uint16_t discarded = 0;
    const uint16_t kMaxSyncDiscard = 1024;

    // We use Serial1.peek() directly to bypass any abstraction limitations
    while (Serial1.available() > 0 && Serial1.peek() != 0x04 && discarded < kMaxSyncDiscard)
    {
        Serial1.read(); // Discard exactly 1 byte of verified ASCII bootloader garbage
        discarded++;
    }

    if (discarded > 0)
    {
        Serial.printf("[SYNC SUCCESS]\tDropped %d bootloader/garbage bytes and aligned stream to 0x04.\n", discarded);
    }

    // Now check if we have enough bytes left to safely extract a full 8-byte header
    if (Serial1.available() < sizeof(fpc_frame_hdr_t))
    {
        return FPC_RESULT_IO_NO_DATA; // Wait for the remaining header bytes to land
    }

    fpc_frame_hdr_t frameHeader;
    uint8_t *raw = (uint8_t *)&frameHeader;

    _comm->beginRead();
    fpc_result_t rc = _comm->read(raw, sizeof(fpc_frame_hdr_t));

    if (rc == FPC_RESULT_IO_NO_DATA)
    {
        _comm->endRead();
        return FPC_RESULT_OK;
    }
    else if (rc != FPC_RESULT_OK)
    {
        _comm->endRead();
        return rc;
    }

    // Double-check structural header validation constraints
    bool valid = (frameHeader.version == FPC_FRAME_PROTOCOL_VERSION) &&
                 ((frameHeader.flags & FPC_FRAME_FLAG_SENDER_FW_APP) != 0) &&
                 (frameHeader.type == FPC_FRAME_TYPE_CMD_RESPONSE ||
                  frameHeader.type == FPC_FRAME_TYPE_CMD_EVENT) &&
                 (frameHeader.payload_size <= MAX_HOST_PACKET_SIZE_DEFAULT);

    if (!valid)
    {
        // False alarm or corruption: clear just 1 byte to advance alignment on next execution loop
        Serial1.read(); 
        _comm->endRead();
        return FPC_RESULT_IO_BAD_DATA;
    }

    // --- Wait for the payload to finish arriving over UART ---
    unsigned long payloadStartMs = millis();
    const unsigned long kPayloadTimeoutMs = 1500; 
    
    while (Serial1.available() < frameHeader.payload_size)
    {
        if ((millis() - payloadStartMs) > kPayloadTimeoutMs)
        {
            Serial.printf("[ERROR]\tPayload timeout! Expected %d bytes, but only %d arrived in Serial1 buffer.\n", 
                          frameHeader.payload_size, Serial1.available());
            _comm->endRead();
            return FPC_RESULT_IO_BAD_DATA;
        }
        delay(1); 
    }

    // Allocate and read the payload safely
    uint8_t framePayload[frameHeader.payload_size];

    rc = _comm->read(framePayload, frameHeader.payload_size);
    _comm->endRead();
    
    if (rc != FPC_RESULT_OK)
        return rc;

    if (flushNone)
    {
        if (checkForNoneEvent(framePayload, frameHeader.payload_size))
            return FPC_RESULT_OK;
    }

    return parseCommand(framePayload, frameHeader.payload_size);
}


//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::setLED(bool ledOn)
{
    if (_comm == nullptr)
        return FPC_RESULT_WRONG_STATE;

    fpc_result_t rc = requestSetGPIO(SPARKFUN_FPC2534_LED_PIN, GPIO_CONTROL_MODE_OUTPUT_PP,
                                     ledOn ? GPIO_CONTROL_STATE_SET : GPIO_CONTROL_STATE_RESET);
    if (rc == FPC_RESULT_OK)
    {
        delay(50);
        flushNoneEvent();
    }
    return rc;
}

//--------------------------------------------------------------------------------------------
fpc_result_t sfDevFPC2534::flushNoneEvent(void)
{
    if (_comm == nullptr)
        return FPC_RESULT_WRONG_STATE;

    return processNextResponse(true);
}
