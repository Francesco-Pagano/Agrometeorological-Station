#pragma once
#include <Arduino.h>

// Connette il WiFi (bloccante finche' non riesce) e avvia il client NTP.
// Da chiamare in setup().
void wifiTimeBegin();

// Da chiamare ad ogni loop() quando il WiFi e' connesso: aggiorna l'orario NTP.
void timeUpdate();

// Epoch Unix (UTC) corrente secondo l'NTP client.
unsigned long getEpochTime();
