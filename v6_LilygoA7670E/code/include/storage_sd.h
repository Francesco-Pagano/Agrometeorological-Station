#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <driver/gpio.h>
#include <math.h>
#include "config.h"

#define VALID_METEO (1 << 0)
#define VALID_LEAF  (1 << 1)
#define VALID_SOIL  (1 << 2)
#define VALID_DIAG  (1 << 3)
#define VALID_ALL_MASK (VALID_METEO | VALID_LEAF | VALID_SOIL | VALID_DIAG)

#define HW_FLAG_SD_OK     (1 << 0)
#define HW_FLAG_CC1101_OK (1 << 1)
#define HW_FLAG_WIFI_OK   (1 << 2)

struct __attribute__((packed)) SensorRecord {
  uint32_t timestamp;
  uint8_t  valid_mask;
  float    temp;
  float    hum;
  float    wind_speed;
  float    wind_gust;
  float    wind_dir;
  float    rain_mm;
  float    light;
  float    uv;
  int8_t   low_bat;
  float    leaf_t;
  float    leaf_w;
  float    soil_t;
  float    soil_h;
  int8_t   csq;
  uint8_t  reset_reason;
  uint8_t  wdt_reboots;
  uint8_t  hw_flags;
  uint16_t free_heap_kb;
};

Preferences sdPrefs;
size_t queueCursor = 0;
bool queueCursorDirty = false;
bool sdMounted = false;
bool cc1101Present = false;

void deselectSDAndReleaseMiso() {
  pinMode(CC1101_CS_PIN, OUTPUT);
  digitalWrite(CC1101_CS_PIN, HIGH);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);

  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  for (uint8_t i = 0; i < 4; i++) {
    SPI.transfer(0xFF);
  }
  SPI.endTransaction();
  pinMode(SPI_MISO_PIN, INPUT_PULLUP);
  gpio_pullup_en((gpio_num_t)SPI_MISO_PIN);
}

bool cc1101WaitMisoLow(unsigned long timeoutMs = 15) {
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  pinMode(CC1101_CS_PIN, OUTPUT);
  pinMode(SPI_MISO_PIN, INPUT_PULLUP);
  gpio_pullup_en((gpio_num_t)SPI_MISO_PIN);

  digitalWrite(CC1101_CS_PIN, LOW);
  unsigned long t0 = millis();
  while (digitalRead(SPI_MISO_PIN) == HIGH) {
    if (millis() - t0 > timeoutMs) {
      digitalWrite(CC1101_CS_PIN, HIGH);
      return false;
    }
  }
  digitalWrite(CC1101_CS_PIN, HIGH);
  return true;
}

bool cc1101ChipResponds() {
  deselectSDAndReleaseMiso();

  digitalWrite(CC1101_CS_PIN, LOW);
  delayMicroseconds(10);
  digitalWrite(CC1101_CS_PIN, HIGH);
  delayMicroseconds(40);

  return cc1101WaitMisoLow(50);
}

bool recordLooksValid(const SensorRecord& r) {
  if (r.timestamp < MIN_VALID_EPOCH || r.timestamp > MAX_VALID_EPOCH) return false;
  if ((r.valid_mask & (VALID_METEO | VALID_LEAF | VALID_SOIL)) == 0) return false;
  if ((r.valid_mask & ~VALID_ALL_MASK) != 0) return false;
  if (r.low_bat != 0 && r.low_bat != 1) return false;

  const float vals[] = {
    r.temp, r.hum, r.wind_speed, r.wind_gust, r.wind_dir,
    r.rain_mm, r.light, r.uv, r.leaf_t, r.leaf_w, r.soil_t, r.soil_h
  };
  for (float x : vals) {
    if (!isfinite(x) || fabsf(x) > 250000.0f) return false;
  }
  return true;
}

void acquireSD() {
  if (cc1101Present) {
    if (cc1101WaitMisoLow(15)) {
      ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    } else {
      cc1101Present = false;
      LOG_PRINTLN("[CC1101] Timeout MISO in acquireSD: chip disabilitato.");
    }
  }
  digitalWrite(CC1101_CS_PIN, HIGH);
  digitalWrite(SD_CS_PIN, HIGH);

  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
  gpio_pullup_en((gpio_num_t)SPI_MISO_PIN);
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  SPI.transfer(0xFF);
  SPI.transfer(0xFF);
  SPI.endTransaction();
}

void releaseSD() {
  deselectSDAndReleaseMiso();

  if (cc1101Present) {
    if (cc1101WaitMisoLow(15)) {
      ELECHOUSE_cc1101.SetRx();
    } else {
      cc1101Present = false;
      LOG_PRINTLN("[CC1101] Timeout MISO in releaseSD: chip disabilitato.");
    }
  }
}

bool ensureSDMounted() {
  if (sdMounted) return true;
  SD.end();
  delay(20);
  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
  gpio_pullup_en((gpio_num_t)SPI_MISO_PIN);
  sdMounted = SD.begin(SD_CS_PIN, SPI, 4000000);
  if (sdMounted) {
    LOG_PRINTLN("[SD] Remount MicroSD riuscito.");
  }
  return sdMounted;
}

bool initStorageSD() {
  pinMode(CC1101_CS_PIN, OUTPUT);
  digitalWrite(CC1101_CS_PIN, HIGH);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  pinMode(SPI_MISO_PIN, INPUT_PULLUP);
  gpio_pullup_en((gpio_num_t)SPI_MISO_PIN);

  sdPrefs.begin("auro_sd", false);
  queueCursor = sdPrefs.getUInt("cursor", 0);
  queueCursorDirty = false;

  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
  gpio_pullup_en((gpio_num_t)SPI_MISO_PIN);
  if (!SD.begin(SD_CS_PIN, SPI, 4000000)) {
    LOG_PRINTLN("[SD] Primo mount fallito, riprovo a 1 MHz...");
    SD.end();
    delay(100);
    if (!SD.begin(SD_CS_PIN, SPI, 1000000)) {
      LOG_PRINTLN("[SD] ERRORE: MicroSD non rilevata o non formattata in FAT32!");
      sdMounted = false;
      releaseSD();
      return false;
    }
  }

  sdMounted = true;
  LOG_PRINTLN("[SD] MicroSD montata correttamente.");

  if (!SD.exists(SD_HISTORY_FILE)) {
    File csv = SD.open(SD_HISTORY_FILE, FILE_WRITE);
    if (csv) {
      csv.println("timestamp,temp,hum,wind_speed,wind_gust,wind_dir,rain_mm,light,uv,low_bat,leaf_t,leaf_w,soil_t,soil_h");
      csv.flush();
      csv.close();
    }
  }

  if (SD.exists(SD_QUEUE_FILE)) {
    File q = SD.open(SD_QUEUE_FILE, FILE_READ);
    size_t fSize = q ? q.size() : 0;
    if (q) q.close();

    if (queueCursor % sizeof(SensorRecord) != 0) {
      queueCursor = (queueCursor / sizeof(SensorRecord)) * sizeof(SensorRecord);
      sdPrefs.putUInt("cursor", queueCursor);
    }
    if (queueCursor >= fSize) {
      if (SD.remove(SD_QUEUE_FILE)) {
        queueCursor = 0;
        LOG_PRINTLN("[SD] Coda arretrati azzerata.");
      } else {
        queueCursor = fSize;
        LOG_PRINTLN("[SD] Rimozione coda fallita, riprovo al prossimo commit.");
      }
      sdPrefs.putUInt("cursor", queueCursor);
    }
  } else if (queueCursor != 0) {
    queueCursor = 0;
    sdPrefs.putUInt("cursor", 0);
  }

  releaseSD();
  return true;
}

size_t buildJsonPayload(const SensorRecord& r, char* outBuf, size_t maxLen) {
  if (maxLen < 80 || !recordLooksValid(r)) return 0;
  size_t pos = 0;

  #define SAFE_APPEND_JSON(...) do { \
    int n = snprintf(outBuf + pos, maxLen - pos, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= (maxLen - pos)) return 0; \
    pos += (size_t)n; \
  } while (0)

  SAFE_APPEND_JSON("{\"timestamp\":%lu", (unsigned long)r.timestamp);

  if (r.valid_mask & VALID_METEO) {
    SAFE_APPEND_JSON(
      ",\"temp\":%.1f,\"hum\":%.0f,\"wind_speed\":%.2f,\"wind_gust\":%.2f,\"wind_dir\":%.0f,\"rain_mm\":%.2f,\"light\":%.1f,\"uv\":%.0f,\"low_bat\":%d",
      r.temp, r.hum, r.wind_speed, r.wind_gust, r.wind_dir, r.rain_mm, r.light, r.uv, (int)r.low_bat
    );
  }
  if (r.valid_mask & VALID_LEAF) {
    SAFE_APPEND_JSON(",\"leaf_t\":%.2f,\"leaf_w\":%.2f", r.leaf_t, r.leaf_w);
  }
  if (r.valid_mask & VALID_SOIL) {
    SAFE_APPEND_JSON(",\"soil_t\":%.1f,\"soil_h\":%.1f", r.soil_t, r.soil_h);
  }
  #if SEND_DIAG_IN_JSON
    if (r.valid_mask & VALID_DIAG) {
      if (r.csq >= 0 && r.csq <= 31) {
        SAFE_APPEND_JSON(",\"csq\":%d", (int)r.csq);
      }
      SAFE_APPEND_JSON(
        ",\"wdt\":%u,\"rst\":%u,\"heap\":%u,\"hw\":%u",
        (unsigned)r.wdt_reboots, (unsigned)r.reset_reason, (unsigned)r.free_heap_kb, (unsigned)r.hw_flags
      );
    }
  #endif

  SAFE_APPEND_JSON("}");
  #undef SAFE_APPEND_JSON
  return pos;
}

bool saveRecordToSD(const SensorRecord& rec) {
  if (!recordLooksValid(rec)) return false;

  acquireSD();
  if (!ensureSDMounted()) {
    releaseSD();
    return false;
  }

  File q = SD.open(SD_QUEUE_FILE, FILE_APPEND);
  if (!q) {
    sdMounted = false;
    if (ensureSDMounted()) q = SD.open(SD_QUEUE_FILE, FILE_APPEND);
  }

  bool ok = false;
  if (q) {
    size_t rem = q.size() % sizeof(SensorRecord);
    if (rem != 0) {
      size_t padLen = sizeof(SensorRecord) - rem;
      uint8_t ff = 0xFF;
      for (size_t i = 0; i < padLen; i++) q.write(&ff, 1);
    }
    size_t w = q.write((const uint8_t*)&rec, sizeof(SensorRecord));
    q.flush();
    q.close();
    ok = (w == sizeof(SensorRecord));
  } else {
    sdMounted = false;
  }

  if (ok && (rec.valid_mask & (VALID_METEO | VALID_LEAF | VALID_SOIL))) {
    File csv = SD.open(SD_HISTORY_FILE, FILE_APPEND);
    if (csv) {
      csv.printf("%lu,", (unsigned long)rec.timestamp);
      if (rec.valid_mask & VALID_METEO) {
        csv.printf("%.1f,%.0f,%.2f,%.2f,%.0f,%.2f,%.1f,%.0f,%d,",
          rec.temp, rec.hum, rec.wind_speed, rec.wind_gust,
          rec.wind_dir, rec.rain_mm, rec.light, rec.uv, (int)rec.low_bat);
      } else {
        csv.print(",,,,,,,,,");
      }
      if (rec.valid_mask & VALID_LEAF) {
        csv.printf("%.2f,%.2f,", rec.leaf_t, rec.leaf_w);
      } else {
        csv.print(",,");
      }
      if (rec.valid_mask & VALID_SOIL) {
        csv.printf("%.1f,%.1f\n", rec.soil_t, rec.soil_h);
      } else {
        csv.print(",\n");
      }
      csv.flush();
      csv.close();
    }
  }

  releaseSD();
  return ok;
}

size_t getPendingRecordsCount() {
  acquireSD();
  if (!ensureSDMounted() || !SD.exists(SD_QUEUE_FILE)) {
    releaseSD();
    return 0;
  }
  File q = SD.open(SD_QUEUE_FILE, FILE_READ);
  if (!q) {
    sdMounted = false;
    releaseSD();
    return 0;
  }
  size_t fSize = q.size();
  q.close();
  releaseSD();

  if (fSize <= queueCursor) return 0;
  return (fSize - queueCursor) / sizeof(SensorRecord);
}

bool peekNextRecord(SensorRecord& outRec) {
  acquireSD();
  if (!ensureSDMounted() || !SD.exists(SD_QUEUE_FILE)) {
    releaseSD();
    return false;
  }
  File q = SD.open(SD_QUEUE_FILE, FILE_READ);
  if (!q) {
    sdMounted = false;
    releaseSD();
    return false;
  }

  bool ok = false;
  if (queueCursor + sizeof(SensorRecord) <= q.size()) {
    if (q.seek(queueCursor)) {
      ok = (q.read((uint8_t*)&outRec, sizeof(SensorRecord)) == sizeof(SensorRecord));
    }
  }
  q.close();
  releaseSD();
  return ok;
}

void confirmRecordSent() {
  queueCursor += sizeof(SensorRecord);
  queueCursorDirty = true;
}

void commitQueueState() {
  if (!queueCursorDirty) return;

  acquireSD();
  if (ensureSDMounted() && SD.exists(SD_QUEUE_FILE)) {
    File q = SD.open(SD_QUEUE_FILE, FILE_READ);
    if (q) {
      size_t fSize = q.size();
      q.close();
      if (queueCursor >= fSize) {
        if (SD.remove(SD_QUEUE_FILE)) {
          queueCursor = 0;
          LOG_PRINTLN("[SD] Coda arretrati azzerata.");
        } else {
          queueCursor = fSize;
          LOG_PRINTLN("[SD] Rimozione coda fallita, riprovo al prossimo commit.");
        }
      }
    } else {
      sdMounted = false;
    }
  }
  sdPrefs.putUInt("cursor", queueCursor);
  queueCursorDirty = false;
  releaseSD();
}

void skipCorruptedSector() {
  size_t recordEndSector = (queueCursor + sizeof(SensorRecord) - 1) / 512;
  size_t nextSector = (recordEndSector + 1) * 512;
  queueCursor = ((nextSector + sizeof(SensorRecord) - 1) / sizeof(SensorRecord)) * sizeof(SensorRecord);
  queueCursorDirty = true;
  LOG_PRINTF("[SD] Salto settore danneggiato: nuovo cursore a %u byte.\n", (unsigned)queueCursor);
  commitQueueState();
}

void quarantineCorruptedQueue() {
  acquireSD();
  bool moved = false;
  char badName[16] = {0};

  if (ensureSDMounted() && SD.exists(SD_QUEUE_FILE)) {
    uint8_t badIdx = sdPrefs.getUChar("bad_idx", 0);
    for (uint8_t tries = 0; tries < 100; tries++) {
      snprintf(badName, sizeof(badName), "/qbad_%02u.bin", (unsigned)((badIdx + tries) % 100));
      if (!SD.exists(badName)) {
        badIdx = (badIdx + tries) % 100;
        break;
      }
    }
    moved = SD.rename(SD_QUEUE_FILE, badName);
    if (moved) {
      sdPrefs.putUChar("bad_idx", (badIdx + 1) % 100);
    }
  }

  if (moved) {
    queueCursor = 0;
    queueCursorDirty = false;
    sdPrefs.putUInt("cursor", 0);
    LOG_PRINTF("[SD] Coda isolata in %s e cursore azzerato.\n", badName);
  } else {
    LOG_PRINTLN("[SD] Rename quarantena non riuscito: cursore preservato.");
  }

  sdMounted = false;
  releaseSD();
}
