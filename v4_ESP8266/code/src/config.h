#pragma once
#include <Arduino.h>

// Pin
const int WIND_DIR_PIN = A0;
const int RAIN_PIN = 13;
const int WIND_SPEED_PIN = 12;

// Sensors
const float BUCKET_SIZE = 0.2794;
const float WIND_SPEED_FACTOR = 2.4; 
const unsigned long GUST_CALC_INTERVAL = 3000;
const unsigned long MIN_VALID_EPOCH = 1600000000UL;

// Intervals
const unsigned long SEND_INTERVAL = 10UL * 60UL * 1000UL; 
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;
const unsigned long OFFLINE_MAX_LINES = 5000;
const unsigned long REPLAY_GRACE_MS = 15000; 
const unsigned long REPLAY_BATCH_DELAY = 15000; 

// Direction
int getWindDirectionDegrees(int adc);