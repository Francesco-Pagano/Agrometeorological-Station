#include <Arduino.h>

#pragma once

// Wi-FI
const char* WIFI_SSID = "Fastweb";
const char* WIFI_PASSWORD = "password";

// OTA
const char* OTA_HOSTNAME = "Lilygo_Auro";
const char* OTA_PASSWORD = "aUro";

// Server MQTT
const char* MQTT_HOST     = "vpn.fpagano.com";
const uint16_t MQTT_PORT  = 1883;
const char* MQTT_TOPIC    = "sensori/auro/dati";
const char* MQTT_CLIENT   = "lilygo_auro";
const char* MQTT_USER     = ""; // Opzionale
const char* MQTT_PASS     = ""; // Opzionale

// SIM
const char* CELL_APN      = "iliad";
const char* CELL_USER     = "";
const char* CELL_PASS     = "";
