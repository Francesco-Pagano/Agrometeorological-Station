#pragma once
#include <Arduino.h>

// Avvia il logging (seriale). Da chiamare per prima cosa in setup().
void debugLogBegin();

// Scrive un messaggio su: Serial + buffer in RAM (visibile via web) + topic MQTT di debug
void logMsg(const String &msg);

// Restituisce il buffer di log corrente (per la pagina di debug web)
const String &getDebugLog();

// Converte un epoch Unix (UTC) in stringa leggibile, utile per verificare il fuso orario
String epochToUtcString(unsigned long epoch);
