#pragma once
#include <Arduino.h>
#include <time.h>
#include "secrets.h"

#ifndef DEBUG
#define DEBUG 0
#endif

// PIN
static const uint8_t WIND_DIR_PIN   = A0;
static const uint8_t RAIN_PIN       = 13;
static const uint8_t WIND_SPEED_PIN = 12;

// SENSORI
static const float         BUCKET_SIZE          = 0.2794f;
static const float         WIND_SPEED_FACTOR    = 2.4f;
static const unsigned long RAIN_DEBOUNCE_MS     = 200UL;
static const unsigned long WIND_DEBOUNCE_MS     = 10UL;
static const unsigned long GUST_CALC_INTERVAL   = 3000UL;
static const unsigned long WIND_DIR_SAMPLE_MS   = 5000UL;
static const unsigned long BME_SAMPLE_INTERVAL  = 60000UL;
static const unsigned long BME_RETRY_INTERVAL   = 300000UL;

// INTERVALLI & TIMEOUT
static const uint32_t      SEND_INTERVAL_SEC            = 10UL * 60UL;
static const unsigned long SEND_INTERVAL_MS             = SEND_INTERVAL_SEC * 1000UL;
static const uint32_t      MIN_VALID_EPOCH              = 1700000000UL;
static const uint32_t      LAST_TS_SAVE_INTERVAL_SEC    = 3600UL;

static const unsigned long MQTT_CONNECT_TIMEOUT_MS      = 2500UL;
static const unsigned long MQTT_CONNACK_TIMEOUT_MS      = 6000UL;
static const unsigned long MQTT_PUBACK_TIMEOUT_MS       = 15000UL;
static const unsigned long MQTT_KEEPALIVE_SEC           = 60UL;
static const unsigned long MQTT_PING_INTERVAL_MS        = 30000UL;
static const unsigned long MQTT_PING_TIMEOUT_MS         = 10000UL;
static const unsigned long MQTT_RECONNECT_BASE_MS       = 10000UL;
static const unsigned long MQTT_RECONNECT_MAX_MS        = 30000UL;

static const unsigned long DRAIN_PACE_MS                = 500UL;
static const unsigned long DRAIN_RETRY_MS               = 3000UL;
static const unsigned long NTP_RETRY_WAIT_MS            = 10000UL;

static const unsigned long WIFI_RETRY_MS                = 30000UL;
static const unsigned long WIFI_HARD_RESET_MS           = 30UL * 60UL * 1000UL;
static const unsigned long SUPERVISOR_WIFI_DEAD_MS      = 2UL * 60UL * 60UL * 1000UL;
static const uint8_t       SUPERVISOR_MAX_MQTT_TIMEOUTS = 6;
static const unsigned long QUEUE_RECONCILE_INTERVAL_MS  = 6UL * 60UL * 60UL * 1000UL;

static const uint32_t RECORD_MAGIC           = 0x57383236UL; // "W826"
static const uint32_t SEGMENT_MAX_RECORDS    = 200UL;        // ~7.6 KB per segmento
static const uint32_t MAX_SEGMENTS           = 60UL;         // 12.000 record
static const uint32_t MIN_FREE_FS_BYTES      = 65536UL;      // 64 KB liberi minimi su LittleFS
static const uint32_t META_SAVE_EVERY_RECS   = 5UL;          // Riduce scritture Flash durante il drain
static const size_t   RAM_FALLBACK_CAPACITY  = 12;           // Buffer RAM di emergenza

static const uint8_t  REC_FLAG_BME_VALID     = (1 << 0);

// RECORD BINARIO (38 BYTE)
struct __attribute__((packed)) WeatherRecord {
  uint32_t magic;
  uint32_t timestamp;       // Unix UTC (0 se pre-NTP; valido fino al 2106)
  uint32_t bootId;          // ID sessione boot
  uint32_t uptimeSec;       // Secondi dal boot
  int16_t  temp_x100;       // °C * 100
  uint16_t hum_x100;        // % * 100
  uint32_t pres_x100;       // hPa * 100
  uint32_t gas_x100;        // kOhm * 100
  uint16_t windSpeed_x100;  // km/h * 100
  uint16_t windGust_x100;   // km/h * 100
  uint16_t windDir;         // 0..359°
  uint32_t rain_x100;       // mm * 100
  uint8_t  flags;           // Bitmask
  uint8_t  crc8;
};

static const size_t RECORD_SIZE = sizeof(WeatherRecord);
static_assert(sizeof(WeatherRecord) == 40, "Dimensione WeatherRecord inattesa");

int      getWindDirectionDegrees(int adc);
uint32_t getUptimeSec();
void     logMsg(const char* fmt, ...);

inline bool isTimeValid(uint32_t ts) {
  return ts >= MIN_VALID_EPOCH;
}

inline bool isSystemTimeValid() {
  time_t now = time(nullptr);
  return (now >= (time_t)MIN_VALID_EPOCH);
}
