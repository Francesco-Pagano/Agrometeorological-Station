#pragma once
#include <Arduino.h>

// Configura il pin e l'interrupt dell'anemometro. Da chiamare in setup().
void windSensorBegin();

// Da chiamare ad ogni loop(): aggiorna il calcolo della raffica massima
// ogni GUST_CALC_INTERVAL ms.
void windSensorUpdateGust();

// Legge la velocita' media del vento dall'ultimo invio (km/h) e azzera il contatore.
// elapsedMillis = tempo trascorso dall'ultimo invio, in millisecondi.
float readAndResetWindSpeed(unsigned long elapsedMillis);

// Restituisce la raffica massima registrata dall'ultimo invio, e la azzera.
float readAndResetMaxGust();

// Converte una lettura ADC del sensore di direzione in gradi (0-360),
// oppure -1 se la lettura e' instabile/fuori dai range noti.
int getWindDirectionDegrees(int adc);
