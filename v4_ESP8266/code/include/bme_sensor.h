#pragma once
#include <Arduino.h>

// Inizializza il sensore BME680 (prova indirizzo 0x76 poi 0x77).
// Da chiamare in setup(). Ritorna true se il sensore e' stato trovato.
bool bmeSensorBegin();

bool bmeSensorFound();

// Effettua una lettura; ritorna true se riuscita e valorizza i parametri
// (temperatura in C, umidita' in %, pressione in hPa, resistenza gas in kOhm).
bool bmeSensorRead(float &temperature, float &humidity, float &pressure, float &gas);
