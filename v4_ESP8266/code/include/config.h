#pragma once
#include <Arduino.h>

// ---------- Pin sensori ----------
const int WIND_DIR_PIN = A0;
const int RAIN_PIN = 13;
const int WIND_SPEED_PIN = 12;

// ---------- Costanti fisiche ----------
const float BUCKET_SIZE = 0.2794;              // mm di pioggia per click del pluviometro
const float WIND_KMH_PER_CLICK_PER_SEC = 2.4;  // fattore di conversione dell'anemometro

// ---------- Timing ----------
const unsigned long SEND_INTERVAL = 10UL * 60UL * 1000UL; // invio dati ogni 10 minuti
const unsigned long GUST_CALC_INTERVAL = 3000;             // ricalcolo raffica ogni 3s
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;        // tentativi di riconnessione MQTT ogni 5s

// Attesa dopo la riconnessione MQTT prima di inviare qualsiasi dato (live o
// dal buffer offline): da qui a Telegraf/altri subscriber hanno il tempo di
// ri-sottoscriversi al topic dopo un riavvio del broker.
const unsigned long REPLAY_GRACE_MS = 90000;

// ---------- Storage offline ----------
const char *const OFFLINE_DATA_FILE = "/offline_data.jsonl";
const unsigned long OFFLINE_MAX_LINES = 5000; // safety cap sul file di buffer

// ---------- Debug ----------
// NB: deve stare FUORI dall'albero di topic sottoscritto da Telegraf
// (es. "sensori/#"), altrimenti i messaggi di log testuali vengono
// interpretati come JSON e mandano in errore il parser.
const char *const DEBUG_TOPIC = "debug/stazione_meteo";
const size_t DEBUG_LOG_MAX_CHARS = 6000;



extern const char* WIFI_SSID;
extern const char* WIFI_PASSWORD;

extern const char* OTA_HOSTNAME;
extern const char* OTA_PASSWORD;

extern const char* mqtt_server;
extern const int mqtt_port;
extern const char* mqtt_topic;
extern const char* mqtt_clientName;
