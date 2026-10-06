#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

extern WiFiClient espClient;
extern PubSubClient mqttClient;

// Inizializza il client MQTT (server/porta). Da chiamare in setup().
void mqttBegin();

// Da chiamare ad ogni loop() quando il WiFi e' connesso: gestisce la
// riconnessione automatica e il mqttClient.loop() interno.
void mqttUpdate();

// Da chiamare quando il WiFi risulta disconnesso, per resettare lo stato
// interno (cosi' alla riconnessione riparte correttamente il grace period).
void mqttNotifyWifiDown();

// Vero se il client e' connesso e sono passati almeno REPLAY_GRACE_MS dalla
// riconnessione: da usare prima di qualsiasi publish "critico" (dati live o
// replay del buffer offline), per dare tempo a Telegraf (o altri subscriber)
// di ri-sottoscriversi al topic dopo un riavvio del broker.
bool mqttReady();
