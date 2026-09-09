#pragma once
#include <Arduino.h>

// Monta LittleFS (con tentativo di format se il mount fallisce).
// Da chiamare in setup(). Ritorna true se il filesystem e' disponibile.
bool storageBegin();

// Vero se LittleFS e' montato correttamente
bool storageIsMounted();

// Conta quante righe (misurazioni) sono in coda nel buffer offline
unsigned long countOfflineLines();

// Salva un payload JSON nel buffer offline (se c'e' spazio e il FS e' montato)
void saveOfflineData(const String &payload);

// Rilegge il buffer offline e ripubblica tutto via MQTT; riscrive il file
// tenendo solo le righe che non e' riuscito a inviare.
// Da chiamare solo quando mqttReady() e' vero (vedi mqtt_manager.h).
void sendOfflineData();
