#pragma once
#include <Arduino.h>

// DEBUG (0 = Disattivato, 1 = Attivo)
#define DEBUG_LOG 0

#if DEBUG_LOG
  #define LOG_BEGIN(baud) Serial.begin(baud)
  #define LOG_PRINT(x)    Serial.print(x)
  #define LOG_PRINTLN(x)  Serial.println(x)
  #define LOG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
  #define LOG_BEGIN(baud) ((void)0)
  #define LOG_PRINT(x)    ((void)0)
  #define LOG_PRINTLN(x)  ((void)0)
  #define LOG_PRINTF(...) ((void)0)
#endif

// MODE (SOLO UNA)
#define RUN_MAIN
//#define RUN_TEST_LEAF
//#define RUN_TEST_SOIL
//#define RUN_TEST_CC1101

// FREQUENZA CPU ESP32
#define ESP32_CPU_FREQ_MHZ 80

// PIN ALIMENTAZIONE PERIFERICHE
#define PIN_POWER_ON    12  // Alimentazione periferiche (Modem + SD)

// 4G A7670E (Serial1)
#define MODEM_TX_PIN    26
#define MODEM_RX_PIN    27
#define MODEM_PWRKEY    4
#define MODEM_RESET_PIN 5
#define MODEM_DTR_PIN   25

// RS485 (Serial2)
#define RS485_RX_PIN    36
#define RS485_TX_PIN    19
#define LEAF_SENSOR_ID  1
#define SOIL_SENSOR_ID  2

// SPI Condiviso (MicroSD + CC1101)
#define SPI_SCK_PIN     14
#define SPI_MISO_PIN    2
#define SPI_MOSI_PIN    15
#define SD_CS_PIN       13  // CS MicroSD
#define CC1101_CS_PIN   32  // CS CC1101
#define CC1101_GDO0_PIN 21
#define CC1101_GDO2_PIN 22

// TEMPISTICHE E BATCHING
#define INTERVAL_RS485_MS            15000UL    // Alterna Foglia e Terreno ogni 15s (1 lettura ogni 30s per sensore)
#define INTERVAL_CC1101_POLL_MS      50UL       // Interroga il FIFO CC1101 ogni 50ms (20 Hz)
#define INTERVAL_SAVE_MS             600000UL   // Salvataggio medie su SD ogni 10 min (600 s)
#define MIN_SAMPLE_WINDOW_MS         60000UL    // Finestra minima di campionamento prima di un salvataggio allineato (60 s)
#define INTERVAL_FLUSH_FAST_MS       30000UL    // Invio rapido se ci sono ancora arretrati in coda: 30 sec
#define INTERVAL_FLUSH_BASE_MS       300000UL   // Invio coda SD normale ogni 5 min
#define INTERVAL_FLUSH_MAX_MS        1800000UL  // Backoff max a server spento: 30 min
#define INTERVAL_CLOCK_SYNC_MS       86400000UL // Risincronizza orologio ogni 24 ore
#define INTERVAL_CLOCK_RETRY_MS      1800000UL  // Riprova sync orologio dopo 30 min se già sincronizzato
#define MODEM_HARD_RESET_COOLDOWN_MS 10800000UL // Hard-reset modem max 1 volta ogni 3 ore
#define SD_ERROR_MIN_SPACING_MS      600000UL   // Distanza minima (10 min) tra errori SD contati
#define SD_MAX_SECTOR_SKIPS          3          // Salti di settore (512B) prima di mettere in quarantena la coda
#define MAX_BATCH_PER_SESSION        40         // Max record inviati in una singola sessione TCP/MQTT
#define MAX_INVALID_SKIP_PER_CYCLE   50         // Max record corrotti saltati per singolo ciclo
#define RAM_RING_CAPACITY            18         // Buffer circolare RAM (18 record = 3 ore di autonomia)

// TELEMETRIA DIAGNOSTICA NEL JSON (0 = Disattivata, 1 = Attiva)
#define SEND_DIAG_IN_JSON            0

// Hotspot/OTA
#define WIFI_SCAN_INTERVAL_MS        60000UL    // Cerca l'hotspot ogni 60 secondi
#define WIFI_CONNECT_TIMEOUT_MS      6000UL     // Timeout aggancio WiFi: 6 secondi

// File su SD e Watchdog
#define SD_QUEUE_FILE   "/queue_v2.bin"
#define SD_HISTORY_FILE "/history.csv"
#define MIN_VALID_EPOCH 1700000000UL
#define MAX_VALID_EPOCH (MIN_VALID_EPOCH + (15UL * 365UL * 86400UL))
#define WDT_TIMEOUT_SEC 180                     // Watchdog a 180s
