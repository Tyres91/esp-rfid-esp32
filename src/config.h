#pragma once
#include <Arduino.h>

struct AppConfig {
  bool   accessPointMode = false;
  String ap_ssid  = "esp-rfid-setup";
  String ap_pass  = "12345678";
  String wifi_ssid = "";
  String wifi_pass = "";

  String hostname = "esp-rfid";
  String mqtt_host = "";
  uint16_t mqtt_port = 1883;
};

extern AppConfig config;

// Default pins for ESP32-WROOM-32
static constexpr int PIN_WIEGAND_D0 = 4;
static constexpr int PIN_WIEGAND_D1 = 5;
static constexpr int PIN_RELAY      = 25;
static constexpr int PIN_BUZZER     = 26;
static constexpr int PIN_LED        = 2;