#include <Arduino.h>
#include "config.h"
#include "secrets.h"
#include "debug_log.h"
#include "wifi_time.h"
#include "mqtt_manager.h"
#include "offline_storage.h"
#include "rain_sensor.h"
#include "wind_sensor.h"
#include "bme_sensor.h"
#include "ota_manager.h"
#include "debug_webserver.h"

static unsigned long lastSendTime = 0;
static int lastValidWindDir = 0;

void setup() {
  debugLogBegin();
  logMsg("\n[BOOT] Avvio stazione meteo...");

  storageBegin();

  rainSensorBegin();
  windSensorBegin();
  bmeSensorBegin();

  wifiTimeBegin();
  otaBegin();
  mqttBegin();
  debugWebServerBegin();

  lastSendTime = millis();

  unsigned long pending = countOfflineLines();
  if (pending > 0) {
    logMsg("[FS] " + String(pending) + " righe in coda dal riavvio precedente.");
  }
}

void loop() {
  debugWebServerHandle(); // sempre attivo, anche senza connessione al broker MQTT

  if (WiFi.status() == WL_CONNECTED) {
    otaHandle();
    timeUpdate();
    mqttUpdate();

    if (mqttReady()) {
      sendOfflineData();
    }
  } else {
    mqttNotifyWifiDown();
  }

  windSensorUpdateGust();

  unsigned long currentMillis = millis();

  if (currentMillis - lastSendTime >= SEND_INTERVAL) {
    float t = 0, h = 0, p = 0, g = 0;
    bmeSensorRead(t, h, p, g);

    int adcVal = analogRead(WIND_DIR_PIN);
    int wDir = getWindDirectionDegrees(adcVal);
    if (wDir != -1) {
      lastValidWindDir = wDir;
    } else {
      wDir = lastValidWindDir;
    }

    float wSpeed = readAndResetWindSpeed(currentMillis - lastSendTime);
    float maxGust = readAndResetMaxGust();
    float rainMM = readAndResetRainMM();

    unsigned long epochTime = getEpochTime();

    String json = "{";
    json += "\"timestamp\":" + String(epochTime) + ",";
    json += "\"wind_dir\":" + String(wDir) + ",";
    json += "\"wind_speed\":" + String(wSpeed, 2) + ",";
    json += "\"wind_gust\":" + String(maxGust, 2) + ",";
    json += "\"rain_mm\":" + String(rainMM, 2) + ",";
    json += "\"temp\":" + String(t, 2) + ",";
    json += "\"hum\":" + String(h, 2) + ",";
    json += "\"pres\":" + String(p, 2) + ",";
    json += "\"gas\":" + String(g, 2);
    json += "}";

    bool sent = false;
    if (mqttReady()) {
      sent = mqttClient.publish(mqtt_topic, json.c_str());
      if (!sent) {
        logMsg("[MQTT] publish() fallito nonostante connessione attiva.");
      }
    }

    // Salva offline solo se NON e' stato inviato con successo,
    // e solo se l'orario e' plausibile (NTP sincronizzato)
    if (!sent) {
      if (epochTime > 1600000000) {
        saveOfflineData(json);
      } else {
        logMsg("[NTP] Orario non valido (" + String(epochTime) + "), dato scartato (non salvato offline).");
      }
    } else {
      logMsg("[MQTT] Dato inviato (" + epochToUtcString(epochTime) + "): " + json);
    }

    lastSendTime = currentMillis;
  }
}
