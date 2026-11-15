#include <Arduino.h>

#pragma once

// Wi-FI
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// OTA
const char* OTA_HOSTNAME = "YOUR_OTA_HOSTNAME";
const char* OTA_PASSWORD = "YOUR_OTA_PASSWORD";

// Server MQTT
const char* MQTT_HOST     = "YOUR_MQTT_HOST";
const uint16_t MQTT_PORT  = 1883;
const char* MQTT_TOPIC    = "YOUR_MQTT_TOPIC";
const char* MQTT_CLIENT   = "YOUR_MQTT_CLIENT";
const char* MQTT_USER     = ""; // Opzionale
const char* MQTT_PASS     = ""; // Opzionale

// SIM
const char* CELL_APN      = "YOUR_CELL_APN";
const char* CELL_USER     = "";
const char* CELL_PASS     = "";
