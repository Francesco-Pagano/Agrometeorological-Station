#pragma once
#include <Arduino.h>

// Configura il pin e l'interrupt del pluviometro. Da chiamare in setup().
void rainSensorBegin();

// Legge i click accumulati dall'ultima chiamata, azzera il contatore,
// e restituisce i millimetri di pioggia corrispondenti.
float readAndResetRainMM();
