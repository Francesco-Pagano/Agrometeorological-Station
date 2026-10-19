#include "Arduino.h"

const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* OTA_HOSTNAME  = "YOUR_OTA_HOSTNAME";
const char* OTA_PASSWORD  = "YOUR_OTA_PASSWORD";

const char* mqtt_server = "YOUR_MQTT_SERVER";
const int mqtt_port = 1883;
const char* mqtt_topic = "YOUR_MQTT_TOPIC";
const char* mqtt_clientName = "YOUR_MQTT_CLIENT_NAME";

String debugTopic = "YOUR_MQTT_DEBUG_TOPIC";
const char* offlineDataFile = "/offline_data.jsonl";
const char* offlineTempFile = "/offline_temp.jsonl";
