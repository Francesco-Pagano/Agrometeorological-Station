#include "mqtt_manager.h"
#include "config.h"
#include "debug_log.h"

WiFiClient espClient;
PubSubClient mqttClient(espClient);

static unsigned long lastMqttAttempt = 0;
static unsigned long mqttConnectedAt = 0;
static bool wasMqttConnected = false;

void mqttBegin() {
  mqttClient.setServer(mqtt_server, mqtt_port);
}

static void connectMQTT() {
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;
  if (now - lastMqttAttempt < MQTT_RECONNECT_INTERVAL) return;

  lastMqttAttempt = now;
  if (mqttClient.connect("StazioneMeteoESP8266")) {
    logMsg("[MQTT] Connessione OK");
  } else {
    logMsg("[MQTT] Connessione FALLITA, rc=" + String(mqttClient.state()));
  }
}

void mqttUpdate() {
  bool isConnected = mqttClient.connected();

  if (!isConnected) {
    connectMQTT();
  } else {
    mqttClient.loop();

    if (!wasMqttConnected) {
      mqttConnectedAt = millis();
      logMsg("[MQTT] Connesso, attendo " + String(REPLAY_GRACE_MS / 1000) + "s prima di inviare dati...");
    }
  }

  wasMqttConnected = isConnected;
}

void mqttNotifyWifiDown() {
  wasMqttConnected = false;
}

bool mqttReady() {
  return mqttClient.connected() && (millis() - mqttConnectedAt >= REPLAY_GRACE_MS);
}
