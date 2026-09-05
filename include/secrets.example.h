/**
 * @file    secrets.example.h
 * @brief   Template for include/secrets.h, which is deliberately untracked.
 *
 * Copy this file to include/secrets.h and fill in your own values:
 *
 *     cp include/secrets.example.h include/secrets.h
 *
 * connectivity.cpp uses __has_include, so the project still compiles without
 * secrets.h - it just comes up with WiFi disabled and the webhook alert stubbed
 * out, which is the right default for anyone cloning the repo to read it.
 */

#ifndef SECRETS_H
#define SECRETS_H

#define WIFI_SSID       "your-network"
#define WIFI_PASS       "your-password"
#define WEBHOOK_URL     "https://example.com/your/webhook/endpoint"

#endif // SECRETS_H
