#pragma once

// Keep credentials and deployment-specific endpoints in config.local.h, which is
// intentionally not committed. Copy config.example.h to config.local.h to start.
#ifdef __has_include
#if __has_include("config.local.h")
#include "config.local.h"
#endif
#endif

#ifndef LUMI_WIFI_SSID
#define LUMI_WIFI_SSID "P301"
#endif
#ifndef LUMI_WIFI_PASSWORD
#define LUMI_WIFI_PASSWORD "123abc98"
#endif
#ifndef LUMI_SERVER_HOST
#define LUMI_SERVER_HOST "192.168.1.120"
#endif
#ifndef LUMI_SERVER_PORT
#define LUMI_SERVER_PORT 8001
#endif
#ifndef LUMI_DEVICE_CODE
// WARNING: fallback default — nếu 2 thiết bị cùng dùng code này, server sẽ
// kick lẫn nhau. Định nghĩa LUMI_DEVICE_CODE riêng trong config.local.h.
#define LUMI_DEVICE_CODE "lumi-dev-01"
#pragma message("LUMI_DEVICE_CODE not set in config.local.h — using fallback lumi-dev-01. Two devices with the same code will disconnect each other.")
#endif

// TLS must be enabled for any production deployment. When it is enabled, provide
// the issuing CA certificate in config.local.h; setInsecure() is never used.
#ifndef LUMI_USE_TLS
#define LUMI_USE_TLS 0
#endif
#if LUMI_USE_TLS && !defined(LUMI_TLS_ROOT_CA)
#error "LUMI_TLS_ROOT_CA must be defined when LUMI_USE_TLS is enabled"
#endif

// Enter a verified FREE ESP32-S3 GPIO. Do not guess: -1 leaves the LED output off.
#define LUMI_LED_DATA_PIN 42
#define LUMI_LED_COUNT 30
// Set a verified GPIO for an active-low interrupt button. Keep -1 when the
// button is not wired; firmware will then use VAD only.
#ifndef LUMI_INTERRUPT_BUTTON_PIN
#define LUMI_INTERRUPT_BUTTON_PIN -1
#endif

// ES8311 / I2S — Waveshare ESP32-S3-Touch-AMOLED-1.8 reference design.
#define LUMI_ES8311_SDA 15
#define LUMI_ES8311_SCL 14
#define LUMI_I2S_MCLK 16
#define LUMI_I2S_BCLK 9
#define LUMI_I2S_WS 45
#define LUMI_I2S_DOUT 8
#define LUMI_I2S_DIN 10
#define LUMI_ES8311_PA_ENABLE 46
#define LUMI_AUDIO_SAMPLE_RATE 16000
#define LUMI_TTS_SAMPLE_RATE 24000
#define LUMI_VAD_THRESHOLD 500
#define LUMI_VAD_SILENCE_MS 1500
#define LUMI_MAX_RECORD_SECONDS 8

#define LUMI_ENABLE_AUDIO 1
#define LUMI_ENABLE_DISPLAY 0
