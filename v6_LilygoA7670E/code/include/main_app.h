#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <ModbusMaster.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <driver/gpio.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_log.h>
#include "config.h"
#include "secrets.h"
#include "storage_sd.h"
#include "modem_4g.h"

#define RTC_STATE_MAGIC 0xA7670E03UL
RTC_NOINIT_ATTR uint32_t rtcStateMagic;
RTC_NOINIT_ATTR uint8_t  rtcConsecutiveNetFailures;
RTC_NOINIT_ATTR uint8_t  rtcWdtReboots;
RTC_NOINIT_ATTR uint8_t  rtcLastResetReason;
RTC_NOINIT_ATTR uint16_t rtcLastRainRaw;
RTC_NOINIT_ATTR bool     rtcFirstRainReading;

ModbusMaster modbus;

unsigned long lastClockSyncMillis = 0;
unsigned long lastClockSyncAttemptMillis = 0;
bool clockEverSynced = false;
uint32_t lastSavedSlot = 0;

enum WifiState : uint8_t { WIFI_STATE_OFF, WIFI_STATE_CONNECTING, WIFI_STATE_CONNECTED };
WifiState wifiState = WIFI_STATE_OFF;
unsigned long wifiTimerMillis = 0;
bool otaRunning = false;
bool otaUpdating = false;
uint16_t mqttPacketId = 1;

SensorRecord ramRing[RAM_RING_CAPACITY];
uint8_t ramRingHead  = 0;
uint8_t ramRingCount = 0;

void incrementNetFailuresSaturated() {
  if (rtcConsecutiveNetFailures < 250) {
    rtcConsecutiveNetFailures++;
  }
}

void updateFlushBackoffInterval(unsigned long& intervalMs) {
  if (rtcConsecutiveNetFailures <= 1)      intervalMs = INTERVAL_FLUSH_BASE_MS; // 5m
  else if (rtcConsecutiveNetFailures == 2) intervalMs = 600000UL;               // 10m
  else if (rtcConsecutiveNetFailures == 3) intervalMs = 1200000UL;              // 20m
  else                                     intervalMs = INTERVAL_FLUSH_MAX_MS;  // 30m
}

void setSystemEpoch(uint32_t utcEpoch) {
  if (utcEpoch < MIN_VALID_EPOCH || utcEpoch > MAX_VALID_EPOCH) return;
  struct timeval tv = { .tv_sec = (time_t)utcEpoch, .tv_usec = 0 };
  settimeofday(&tv, NULL);
  lastSavedSlot = utcEpoch / (INTERVAL_SAVE_MS / 1000UL);
  clockEverSynced = true;
  lastClockSyncMillis = millis();
  lastClockSyncAttemptMillis = millis();
  LOG_PRINTF("[CLOCK] RTC impostato a UTC Epoch: %lu\n", (unsigned long)utcEpoch);
}

void syncTimeViaNTP() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 3000)) {
    time_t now;
    time(&now);
    if ((uint32_t)now >= MIN_VALID_EPOCH) {
      lastSavedSlot = (uint32_t)now / (INTERVAL_SAVE_MS / 1000UL);
      clockEverSynced = true;
      lastClockSyncMillis = millis();
      lastClockSyncAttemptMillis = millis();
      LOG_PRINTF("[CLOCK] Sync NTP WiFi OK: %lu\n", (unsigned long)now);
    }
  }
}

uint32_t getCurrentEpoch() {
  time_t now;
  time(&now);

  bool needsSync = !clockEverSynced ||
                   ((uint32_t)now < MIN_VALID_EPOCH) ||
                   (millis() - lastClockSyncMillis >= INTERVAL_CLOCK_SYNC_MS);

  if (needsSync) {
    unsigned long retryCooldown = clockEverSynced ? INTERVAL_CLOCK_RETRY_MS : INTERVAL_FLUSH_BASE_MS;
    bool canAttemptNow = (lastClockSyncAttemptMillis == 0) ||
                         (millis() - lastClockSyncAttemptMillis >= retryCooldown);

    if (canAttemptNow) {
      lastClockSyncAttemptMillis = millis();
      if (wifiState == WIFI_STATE_CONNECTED) {
        syncTimeViaNTP();
        time(&now);
      }
      if (!clockEverSynced || (millis() - lastClockSyncMillis >= INTERVAL_CLOCK_SYNC_MS)) {
        uint32_t cellEpoch = getEpochFromModem();
        if (cellEpoch >= MIN_VALID_EPOCH) {
          setSystemEpoch(cellEpoch);
          time(&now);
        }
      }
    }
  }

  return ((uint32_t)now >= MIN_VALID_EPOCH) ? (uint32_t)now : 0;
}

// Accumulatori
float accTemp = 0, accHum = 0, accWind = 0, accSin = 0, accCos = 0, accGust = 0, accRain = 0;
float accLux = 0, accUv = 0;
int8_t lastLowBat = 0;
uint16_t countMeteo = 0;

float accLeafT = 0, accLeafW = 0;
uint16_t countLeaf = 0;

float accSoilT = 0, accSoilH = 0;
uint16_t countSoil = 0;

// Timer e diagnostica SD
unsigned long lastRs485Millis          = 0;
unsigned long lastCcPollMillis         = 0;
unsigned long lastSaveMillis           = 0;
unsigned long lastFlushMillis          = 0;
unsigned long currentFlushIntervalMs   = INTERVAL_FLUSH_BASE_MS;

unsigned long lastCcResetMillis        = 0;
unsigned long lastMeteoMillis          = 0;
bool readLeafTurn                      = true;
uint8_t consecutiveSdReadErrors        = 0;
uint8_t consecutiveSectorSkips         = 0;
unsigned long lastSdErrorCountedMillis = 0;

static int encodeRemainingLength(uint8_t* buf, int length) {
  int idx = 0;
  do {
    uint8_t d = length % 128;
    length /= 128;
    if (length > 0) d |= 0x80;
    buf[idx++] = d;
  } while (length > 0);
  return idx;
}

bool mqttOpenSession(Client& netClient) {
  while (netClient.available()) netClient.read();

  char uniqueClientId[44];
  uint32_t macLow = (uint32_t)(ESP.getEfuseMac() >> 24);
  snprintf(uniqueClientId, sizeof(uniqueClientId), "%.28s_%06X",
           (strlen(MQTT_CLIENT) > 0) ? MQTT_CLIENT : "lilygo", (unsigned)(macLow & 0xFFFFFF));

  uint16_t cidLen  = strlen(uniqueClientId);
  uint16_t userLen = strlen(MQTT_USER);
  uint16_t passLen = strlen(MQTT_PASS);
  if (userLen > 40) userLen = 40;
  if (passLen > 40) passLen = 40;

  bool useAuth = (userLen > 0 && strcmp(MQTT_USER, "YOUR_MQTT_USER") != 0);
  int remLen = 10 + 2 + cidLen + (useAuth ? (2 + userLen + 2 + passLen) : 0);

  uint8_t connPkt[220];
  int pos = 0;
  connPkt[pos++] = 0x10; // CONNECT
  pos += encodeRemainingLength(&connPkt[pos], remLen);

  uint8_t connFlags = useAuth ? 0xC2 : 0x02; // CleanSession
  const uint8_t vh[] = {0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, connFlags, 0x00, 0x1E}; // KeepAlive 30s
  memcpy(&connPkt[pos], vh, 10); pos += 10;

  connPkt[pos++] = (cidLen >> 8) & 0xFF;
  connPkt[pos++] = cidLen & 0xFF;
  memcpy(&connPkt[pos], uniqueClientId, cidLen); pos += cidLen;

  if (useAuth) {
    connPkt[pos++] = (userLen >> 8) & 0xFF;
    connPkt[pos++] = userLen & 0xFF;
    memcpy(&connPkt[pos], MQTT_USER, userLen); pos += userLen;

    connPkt[pos++] = (passLen >> 8) & 0xFF;
    connPkt[pos++] = passLen & 0xFF;
    memcpy(&connPkt[pos], MQTT_PASS, passLen); pos += passLen;
  }

  netClient.write(connPkt, pos);

  unsigned long t0 = millis();
  while (netClient.available() < 4) {
    esp_task_wdt_reset();
    if (millis() - t0 > 6000) return false;
    delay(5);
  }
  uint8_t connAck[4];
  netClient.read(connAck, 4);
  if (connAck[0] != 0x20 || connAck[3] != 0x00) {
    LOG_PRINTF("[MQTT] CONNACK rifiutato: code=0x%02X\n", connAck[3]);
    return false;
  }
  return true;
}

bool mqttPublishPacketQoS1(Client& netClient, const char* topic, const char* payload) {
  while (netClient.available()) netClient.read();

  uint16_t topLen = strlen(topic);
  uint16_t payLen = strlen(payload);
  if (topLen + payLen > 420) return false;

  uint16_t pktId = mqttPacketId++;
  if (mqttPacketId == 0) mqttPacketId = 1;

  int remLen = 2 + topLen + 2 + payLen;
  uint8_t txBuf[450];
  int p = 0;
  txBuf[p++] = 0x32; // PUBLISH QoS 1
  p += encodeRemainingLength(&txBuf[p], remLen);
  txBuf[p++] = (uint8_t)(topLen >> 8);
  txBuf[p++] = (uint8_t)(topLen & 0xFF);
  memcpy(&txBuf[p], topic, topLen); p += topLen;
  txBuf[p++] = (uint8_t)(pktId >> 8);
  txBuf[p++] = (uint8_t)(pktId & 0xFF);
  memcpy(&txBuf[p], payload, payLen); p += payLen;

  netClient.write(txBuf, p);

  unsigned long t0 = millis();
  while (netClient.available() < 4) {
    esp_task_wdt_reset();
    if (millis() - t0 > 7000) return false;
    delay(5);
  }
  uint8_t pubAck[4];
  netClient.read(pubAck, 4);
  uint16_t rxPid = (pubAck[2] << 8) | pubAck[3];

  return (pubAck[0] == 0x40 && rxPid == pktId);
}

void mqttCloseSession(Client& netClient) {
  const uint8_t discPkt[2] = {0xE0, 0x00};
  netClient.write(discPkt, 2);
  netClient.stop();
}

bool connectTcpWithShortTimeout(bool useCellular, WiFiClient& wClient) {
  for (int attempt = 0; attempt < 2; attempt++) {
    esp_task_wdt_reset();
    bool ok = useCellular ? gsmClient.connect(MQTT_HOST, MQTT_PORT, 15)
                          : wClient.connect(MQTT_HOST, MQTT_PORT, 8000);
    if (ok) return true;
    if (useCellular) gsmClient.stop(); else wClient.stop();
    if (attempt == 0) delay(1200);
  }
  return false;
}

enum FlushResult : uint8_t { FLUSH_OK, FLUSH_NET_FAIL, FLUSH_SD_FAIL };

void handleSdReadError() {
  sdMounted = false;
  acquireSD();
  bool remountOk = ensureSDMounted();
  releaseSD();

  if (!remountOk) {
    LOG_PRINTLN("[SD] Remount fallito: probabile calo di tensione o falso contatto, quarantena sospesa.");
    return;
  }

  unsigned long now = millis();
  if (lastSdErrorCountedMillis == 0 || (now - lastSdErrorCountedMillis >= SD_ERROR_MIN_SPACING_MS)) {
    lastSdErrorCountedMillis = now;
    consecutiveSdReadErrors++;
    LOG_PRINTF("[SD] Errore di lettura persistente post-remount (%u/3).\n", consecutiveSdReadErrors);
    if (consecutiveSdReadErrors >= 3) {
      consecutiveSdReadErrors = 0;
      lastSdErrorCountedMillis = 0;

      if (consecutiveSectorSkips < SD_MAX_SECTOR_SKIPS) {
        consecutiveSectorSkips++;
        LOG_PRINTF("[SD] Tentativo salto settore (%u/%u).\n", consecutiveSectorSkips, (unsigned)SD_MAX_SECTOR_SKIPS);
        skipCorruptedSector();
      } else {
        consecutiveSectorSkips = 0;
        quarantineCorruptedQueue();
      }
    }
  }
}

FlushResult prepareNextValidRecordFromSD(bool& hasValidRecord) {
  hasValidRecord = false;
  uint8_t skipped = 0;

  while (skipped < MAX_INVALID_SKIP_PER_CYCLE && getPendingRecordsCount() > 0) {
    esp_task_wdt_reset();
    SensorRecord rec;
    if (!peekNextRecord(rec)) {
      commitQueueState();
      handleSdReadError();
      return FLUSH_SD_FAIL;
    }
    consecutiveSdReadErrors = 0;
    consecutiveSectorSkips = 0;
    lastSdErrorCountedMillis = 0;

    char jsonBuf[384];
    if (buildJsonPayload(rec, jsonBuf, sizeof(jsonBuf)) == 0) {
      LOG_PRINTLN("[SD] Record invalido saltato prima della connessione.");
      confirmRecordSent();
      skipped++;
      continue;
    }
    hasValidRecord = true;
    break;
  }

  commitQueueState();
  return FLUSH_OK;
}

FlushResult flushBatchFromSD(bool useCellular) {
  WiFiClient wClient;
  if (!connectTcpWithShortTimeout(useCellular, wClient)) {
    LOG_PRINTF("[MQTT] Errore connessione TCP (%s) verso %s:%u\n",
               useCellular ? "4G" : "WiFi", MQTT_HOST, MQTT_PORT);
    return FLUSH_NET_FAIL;
  }

  Client& activeClient = useCellular ? (Client&)gsmClient : (Client&)wClient;
  if (!mqttOpenSession(activeClient)) {
    LOG_PRINTLN("[MQTT] Errore apertura sessione MQTT.");
    activeClient.stop();
    return FLUSH_NET_FAIL;
  }

  uint8_t sentCount = 0;
  FlushResult result = FLUSH_OK;

  for (uint8_t i = 0; i < MAX_BATCH_PER_SESSION; i++) {
    esp_task_wdt_reset();
    if (getPendingRecordsCount() == 0) break;

    SensorRecord rec;
    if (!peekNextRecord(rec)) {
      handleSdReadError();
      result = (sentCount > 0) ? FLUSH_OK : FLUSH_SD_FAIL;
      break;
    }
    consecutiveSdReadErrors = 0;
    consecutiveSectorSkips = 0;
    lastSdErrorCountedMillis = 0;

    char jsonBuf[384];
    size_t jLen = buildJsonPayload(rec, jsonBuf, sizeof(jsonBuf));
    if (jLen == 0) {
      LOG_PRINTLN("[SD] Record invalido saltato.");
      confirmRecordSent();
      continue;
    }

    if (mqttPublishPacketQoS1(activeClient, MQTT_TOPIC, jsonBuf)) {
      LOG_PRINTF("[MQTT] Inviato OK su %s: %s\n", MQTT_TOPIC, jsonBuf);
      confirmRecordSent();
      sentCount++;
      if ((sentCount % 10) == 0) {
        commitQueueState();
      }
      delay(80);
    } else {
      LOG_PRINTLN("[MQTT] Errore PUBACK.");
      result = (sentCount > 0) ? FLUSH_OK : FLUSH_NET_FAIL;
      break;
    }
  }

  commitQueueState();
  mqttCloseSession(activeClient);
  return result;
}

void pushRecordToRamRing(const SensorRecord& rec) {
  if (ramRingCount < RAM_RING_CAPACITY) {
    uint8_t idx = (ramRingHead + ramRingCount) % RAM_RING_CAPACITY;
    ramRing[idx] = rec;
    ramRingCount++;
  } else {
    ramRing[ramRingHead] = rec;
    ramRingHead = (ramRingHead + 1) % RAM_RING_CAPACITY;
  }
  LOG_PRINTF("[RAM] Record accodato in RAM ring (%u/%u).\n", ramRingCount, RAM_RING_CAPACITY);
}

bool sendRamRingOverSession(Client& netClient) {
  if (!mqttOpenSession(netClient)) {
    netClient.stop();
    return false;
  }

  uint8_t sent = 0;
  uint8_t toSend = ramRingCount;
  for (uint8_t i = 0; i < toSend; i++) {
    esp_task_wdt_reset();
    SensorRecord& r = ramRing[ramRingHead];
    char jsonBuf[384];
    if (buildJsonPayload(r, jsonBuf, sizeof(jsonBuf)) == 0) {
      ramRingHead = (ramRingHead + 1) % RAM_RING_CAPACITY;
      ramRingCount--;
      continue;
    }
    if (mqttPublishPacketQoS1(netClient, MQTT_TOPIC, jsonBuf)) {
      LOG_PRINTF("[MQTT-RAM] Inviato OK su %s: %s\n", MQTT_TOPIC, jsonBuf);
      ramRingHead = (ramRingHead + 1) % RAM_RING_CAPACITY;
      ramRingCount--;
      sent++;
      delay(80);
    } else {
      break;
    }
  }

  mqttCloseSession(netClient);
  return (sent > 0 || ramRingCount == 0);
}

bool flushRamRingNetworkBatch() {
  if (ramRingCount == 0) return true;

  WiFiClient wClient;
  if (wifiState == WIFI_STATE_CONNECTED) {
    if (connectTcpWithShortTimeout(false, wClient)) {
      if (sendRamRingOverSession(wClient) && ramRingCount == 0) {
        rtcConsecutiveNetFailures = 0;
        return true;
      }
    }
  }

  if (ramRingCount > 0 && ensureCellularConnected()) {
    bool ok = false;
    if (connectTcpWithShortTimeout(true, wClient)) {
      ok = sendRamRingOverSession(gsmClient);
    }
    sleepModemDTR();
    if (ok) {
      rtcConsecutiveNetFailures = 0;
      return true;
    }
  }

  incrementNetFailuresSaturated();
  recoverModemConnection(rtcConsecutiveNetFailures);
  updateFlushBackoffInterval(currentFlushIntervalMs);
  return false;
}

void flushRamRing(uint32_t currentValidEpoch) {
  if (ramRingCount == 0 || currentValidEpoch < MIN_VALID_EPOCH) return;

  uint32_t nowSec = millis() / 1000UL;

  for (uint8_t i = 0; i < ramRingCount; i++) {
    uint8_t idx = (ramRingHead + i) % RAM_RING_CAPACITY;
    if (ramRing[idx].timestamp < MIN_VALID_EPOCH) {
      uint32_t ageSec = (nowSec >= ramRing[idx].timestamp) ? (nowSec - ramRing[idx].timestamp) : 0;
      ramRing[idx].timestamp = (currentValidEpoch > ageSec + MIN_VALID_EPOCH)
                               ? (currentValidEpoch - ageSec)
                               : currentValidEpoch;
    }
  }

  uint8_t itemsToProcess = ramRingCount;
  for (uint8_t i = 0; i < itemsToProcess; i++) {
    SensorRecord& r = ramRing[ramRingHead];
    if (!recordLooksValid(r)) {
      ramRingHead = (ramRingHead + 1) % RAM_RING_CAPACITY;
      ramRingCount--;
      continue;
    }
    if (saveRecordToSD(r)) {
      ramRingHead = (ramRingHead + 1) % RAM_RING_CAPACITY;
      ramRingCount--;
    } else {
      break;
    }
  }

  if (ramRingCount > 0) {
    flushRamRingNetworkBatch();
  }
}

// CC1101
uint8_t crc8Ws(uint8_t const message[], unsigned nBytes) {
  uint8_t remainder = 0x00;
  for (unsigned b = 0; b < nBytes; ++b) {
    remainder ^= message[b];
    for (unsigned bit = 0; bit < 8; ++bit)
      remainder = (remainder & 0x80) ? (remainder << 1) ^ 0x31 : (remainder << 1);
  }
  return remainder;
}

uint8_t readCc1101RxBytesSafe() {
  uint8_t r1 = ELECHOUSE_cc1101.SpiReadStatus(CC1101_RXBYTES);
  for (uint8_t i = 0; i < 5; i++) {
    uint8_t r2 = ELECHOUSE_cc1101.SpiReadStatus(CC1101_RXBYTES);
    if (r1 == r2) return r1;
    r1 = r2;
  }
  return r1;
}

void configCC1101Main() {
  if (!cc1101ChipResponds()) {
    cc1101Present = false;
    LOG_PRINTLN("[CC1101] Timeout MISO: chip assente o MISO bloccato.");
    return;
  }

  ELECHOUSE_cc1101.setSpiPin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, CC1101_CS_PIN);
  ELECHOUSE_cc1101.setGDO(CC1101_GDO0_PIN, CC1101_GDO2_PIN);
  ELECHOUSE_cc1101.Init();

  uint8_t ver = ELECHOUSE_cc1101.SpiReadStatus(CC1101_VERSION);
  if (ver == 0x00 || ver == 0xFF) {
    cc1101Present = false;
    LOG_PRINTF("[CC1101] Chip non rilevato sul bus SPI (Version: 0x%02X).\n", ver);
    return;
  }
  cc1101Present = true;

  ELECHOUSE_cc1101.setModulation(0);
  ELECHOUSE_cc1101.setMHZ(868.3);
  ELECHOUSE_cc1101.setDRate(17.24);
  ELECHOUSE_cc1101.setRxBW(250.0);
  ELECHOUSE_cc1101.setDeviation(40.0);
  ELECHOUSE_cc1101.setPacketLength(27);
  ELECHOUSE_cc1101.setSyncMode(2);
  ELECHOUSE_cc1101.setSyncWord(0xAA, 0x2D);
  ELECHOUSE_cc1101.setManchester(0);
  ELECHOUSE_cc1101.setPktFormat(0);
  ELECHOUSE_cc1101.setLengthConfig(0);
  ELECHOUSE_cc1101.setCrc(0);
  ELECHOUSE_cc1101.setAdrChk(0);
  ELECHOUSE_cc1101.SetRx();
  LOG_PRINTF("[CC1101] Inizializzato OK (Version: 0x%02X) in ascolto a 868.3 MHz.\n", ver);
}

void decodeWs3500Main(uint8_t* b) {
  uint8_t crc = crc8Ws(&b[1], 15);
  uint8_t checksum = 0;
  for (int n = 1; n < 17; ++n) checksum += b[n];
  if (crc != b[16] || checksum != b[17]) {
    LOG_PRINTF("[CC1101] Scartato pacchetto per CRC/Checksum errato (CRC:%02X!=%02X, CHK:%02X!=%02X)\n",
               crc, b[16], checksum, b[17]);
    return;
  }

  float t = (((b[4] & 0x07) << 8 | b[5]) - 400) * 0.1f;
  int h = b[6];
  if (h < 0 || h > 100 || t < -50 || t > 80) return;

  int w_dir = b[3] | (b[4] & 0x80) << 1;
  float w_avg = (b[7] | (b[4] & 0x10) << 4) * 0.125f * 0.51f;
  float w_gst = b[8] * 0.51f;
  int uv_raw = b[11] << 8 | b[12];
  float lux = (b[13] << 16 | b[14] << 8 | b[15]) * 0.1f;

  static const int uvi_upper[] = { 432, 851, 1210, 1570, 2017, 2450, 2761, 3100, 3512, 3918, 4277, 4650, 5029 };
  int uv_idx = 0;
  while (uv_idx < 13 && uvi_upper[uv_idx] < uv_raw) ++uv_idx;

  accTemp += t;
  accHum += h;
  accWind += w_avg;
  if (w_dir >= 0 && w_dir <= 360) {
    accSin += sinf(w_dir * DEG_TO_RAD);
    accCos += cosf(w_dir * DEG_TO_RAD);
  }
  if (w_gst > accGust) accGust = w_gst;
  accLux += lux;
  accUv += uv_idx;
  lastLowBat = (b[4] & 0x08) >> 3;
  countMeteo++;

  uint16_t rain_raw = (b[9] << 8 | b[10]);
  if (!rtcFirstRainReading && rain_raw != rtcLastRainRaw) {
    float delta = (rain_raw < rtcLastRainRaw) ?
                  (65536 - rtcLastRainRaw + rain_raw) * 0.254f :
                  (rain_raw - rtcLastRainRaw) * 0.254f;
    if (delta < 100.0f) accRain += delta;
  }
  rtcLastRainRaw = rain_raw;
  rtcFirstRainReading = false;
  lastMeteoMillis = millis();

  LOG_PRINTF("[METEO] Ricevuto OK (#%u): T=%.1fC, H=%d%%, Wind=%.2fm/s, Lux=%.1f\n",
             countMeteo, t, h, w_avg, lux);
}

void pollCC1101(unsigned long now) {
  if (now - lastCcPollMillis < INTERVAL_CC1101_POLL_MS) return;
  lastCcPollMillis = now;

  if ((now - lastMeteoMillis) > 1200000UL) {
    configCC1101Main();
    lastMeteoMillis = now;
  }
  if (!cc1101Present) return;

  if ((now - lastCcResetMillis) > 300000UL) {
    if (!cc1101ChipResponds()) {
      cc1101Present = false;
      lastMeteoMillis = now - 1140000UL;
      LOG_PRINTLN("[CC1101] Chip non risponde al reset periodico: riprovo tra 60s.");
      return;
    }
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SFRX);
    ELECHOUSE_cc1101.SetRx();
    lastCcResetMillis = now;
  }

  uint8_t rxb = readCc1101RxBytesSafe();
  if (rxb == 0xFF) {
    if (!cc1101ChipResponds()) {
      cc1101Present = false;
      lastMeteoMillis = now - 1140000UL;
      LOG_PRINTLN("[CC1101] Persa comunicazione SPI: riprovo tra 60s.");
      return;
    }
    ELECHOUSE_cc1101.SetRx();
    return;
  }

  if (rxb & 0x80) {
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SFRX);
    ELECHOUSE_cc1101.SetRx();
  } else if ((rxb & 0x7F) >= 27) {
    uint8_t buf[27];
    ELECHOUSE_cc1101.SpiReadBurstReg(CC1101_RXFIFO, buf, 27);
    if (buf[0] == 0xD4 && buf[1] == 0x24) {
      decodeWs3500Main(buf);
    } else if (buf[0] == 0x24) {
      uint8_t shifted[28];
      shifted[0] = 0x00;
      memcpy(&shifted[1], buf, 27);
      decodeWs3500Main(shifted);
    } else {
      LOG_PRINTF("[CC1101] Ricevuti 27 byte ma header sconosciuto: %02X %02X\n", buf[0], buf[1]);
    }
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SFRX);
    ELECHOUSE_cc1101.SetRx();
  }
}

void pollRS485(unsigned long now) {
  if (now - lastRs485Millis < INTERVAL_RS485_MS) return;
  lastRs485Millis = now;

  if (readLeafTurn) {
    modbus.begin(LEAF_SENSOR_ID, Serial2);
    uint8_t res = modbus.readHoldingRegisters(0x0000, 2);
    if (res == modbus.ku8MBSuccess) {
      float lt = (int16_t)modbus.getResponseBuffer(0) / 100.0f;
      float lw = modbus.getResponseBuffer(1) / 100.0f;
      accLeafT += lt;
      accLeafW += lw;
      countLeaf++;
      LOG_PRINTF("[RS485] Foglia OK (#%u): T=%.2fC, W=%.2f%%\n", countLeaf, lt, lw);
    } else {
      LOG_PRINTF("[RS485] Foglia errore Modbus: 0x%02X\n", res);
    }
  } else {
    modbus.begin(SOIL_SENSOR_ID, Serial2);
    uint8_t res = modbus.readHoldingRegisters(0x0000, 2);
    if (res == modbus.ku8MBSuccess) {
      float st = (int16_t)modbus.getResponseBuffer(0) / 10.0f;
      float sh = modbus.getResponseBuffer(1) / 10.0f;
      accSoilT += st;
      accSoilH += sh;
      countSoil++;
      LOG_PRINTF("[RS485] Terreno OK (#%u): T=%.1fC, H=%.1f%%\n", countSoil, st, sh);
    } else {
      LOG_PRINTF("[RS485] Terreno errore Modbus: 0x%02X\n", res);
    }
  }
  readLeafTurn = !readLeafTurn;
}

void manageHotspotWifi(unsigned long now) {
  switch (wifiState) {
    case WIFI_STATE_OFF:
      if (now - wifiTimerMillis >= WIFI_SCAN_INTERVAL_MS) {
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(false);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        wifiState = WIFI_STATE_CONNECTING;
        wifiTimerMillis = now;
      }
      break;

    case WIFI_STATE_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        LOG_PRINTLN("[WIFI] Hotspot connesso: " + WiFi.localIP().toString());
        ArduinoOTA.begin();
        otaRunning = true;
        wifiState = WIFI_STATE_CONNECTED;
        syncTimeViaNTP();
      } else if (now - wifiTimerMillis >= WIFI_CONNECT_TIMEOUT_MS) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        wifiState = WIFI_STATE_OFF;
        wifiTimerMillis = now;
      }
      break;

    case WIFI_STATE_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        if (otaRunning) { ArduinoOTA.end(); otaRunning = false; }
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        wifiState = WIFI_STATE_OFF;
        wifiTimerMillis = now;
      } else {
        ArduinoOTA.handle();
      }
      break;
  }
}

void tryFlushNextFromSD() {
  if (ramRingCount > 0) {
    uint32_t ep = getCurrentEpoch();
    if (ep >= MIN_VALID_EPOCH) {
      flushRamRing(ep);
    }
  }

  bool hasValidToSend = false;
  FlushResult prepRes = prepareNextValidRecordFromSD(hasValidToSend);
  if (prepRes == FLUSH_SD_FAIL) {
    currentFlushIntervalMs = INTERVAL_FLUSH_BASE_MS;
    return;
  }
  if (!hasValidToSend) {
    currentFlushIntervalMs = (getPendingRecordsCount() > 0)
                             ? INTERVAL_FLUSH_FAST_MS
                             : INTERVAL_FLUSH_BASE_MS;
    return;
  }

  FlushResult res = FLUSH_NET_FAIL;

  if (wifiState == WIFI_STATE_CONNECTED) {
    res = flushBatchFromSD(false);
  }

  if (res == FLUSH_NET_FAIL) {
    if (ensureCellularConnected()) {
      res = flushBatchFromSD(true);
      sleepModemDTR();
    } else {
      res = FLUSH_NET_FAIL;
    }
  }

  if (res == FLUSH_OK) {
    rtcConsecutiveNetFailures = 0;
    currentFlushIntervalMs = (getPendingRecordsCount() > 0)
                             ? INTERVAL_FLUSH_FAST_MS
                             : INTERVAL_FLUSH_BASE_MS;
  } else if (res == FLUSH_SD_FAIL) {
    currentFlushIntervalMs = INTERVAL_FLUSH_BASE_MS;
  } else if (res == FLUSH_NET_FAIL) {
    incrementNetFailuresSaturated();
    recoverModemConnection(rtcConsecutiveNetFailures);
    updateFlushBackoffInterval(currentFlushIntervalMs);
  }
}

void resetAccumulators() {
  accTemp = accHum = accWind = accSin = accCos = accGust = accRain = accLux = accUv = 0;
  countMeteo = 0;
  accLeafT = accLeafW = 0; countLeaf = 0;
  accSoilT = accSoilH = 0; countSoil = 0;
}

void populateDiagnostics(SensorRecord& rec) {
  rec.valid_mask  |= VALID_DIAG;
  rec.csq          = lastKnownCsq;
  rec.reset_reason = rtcLastResetReason;
  rec.wdt_reboots  = rtcWdtReboots;
  rec.free_heap_kb = (uint16_t)(ESP.getFreeHeap() / 1024UL);

  uint8_t flags = 0;
  if (sdMounted)                        flags |= HW_FLAG_SD_OK;
  if (cc1101Present)                    flags |= HW_FLAG_CC1101_OK;
  if (wifiState == WIFI_STATE_CONNECTED) flags |= HW_FLAG_WIFI_OK;
  rec.hw_flags = flags;
}

void handleSaveAndFlush(unsigned long now) {
  bool triggerSave = false;
  uint32_t alignedEpoch = 0;
  const uint32_t slotSec = INTERVAL_SAVE_MS / 1000UL;

  if (clockEverSynced) {
    time_t nowTs;
    time(&nowTs);
    if ((uint32_t)nowTs >= MIN_VALID_EPOCH) {
      uint32_t currentSlot = (uint32_t)nowTs / slotSec;
      if (lastSavedSlot == 0) lastSavedSlot = currentSlot;
      if (currentSlot != lastSavedSlot && (now - lastSaveMillis >= MIN_SAMPLE_WINDOW_MS)) {
        triggerSave = true;
        lastSavedSlot = currentSlot;
        alignedEpoch = currentSlot * slotSec;
      }
    }
  }

  if (!triggerSave && (now - lastSaveMillis >= INTERVAL_SAVE_MS)) {
    triggerSave = true;
  }

  if (triggerSave) {
    lastSaveMillis = now;

    uint8_t mask = 0;
    if (countMeteo > 0) mask |= VALID_METEO;
    if (countLeaf > 0)  mask |= VALID_LEAF;
    if (countSoil > 0)  mask |= VALID_SOIL;

    if (mask != 0) {
      SensorRecord rec;
      memset(&rec, 0, sizeof(rec));
      rec.valid_mask = mask;

      if (mask & VALID_METEO) {
        rec.temp       = accTemp / countMeteo;
        rec.hum        = roundf(accHum / countMeteo);
        rec.wind_speed = accWind / countMeteo;
        rec.wind_gust  = accGust;
        float dir      = atan2f(accSin, accCos) * RAD_TO_DEG;
        if (dir < 0) dir += 360.0f;
        rec.wind_dir   = roundf(dir);
        rec.rain_mm    = accRain;
        rec.light      = accLux / countMeteo;
        rec.uv         = roundf(accUv / countMeteo);
        rec.low_bat    = lastLowBat;
      }
      if (mask & VALID_LEAF) {
        rec.leaf_t     = accLeafT / countLeaf;
        rec.leaf_w     = accLeafW / countLeaf;
      }
      if (mask & VALID_SOIL) {
        rec.soil_t     = accSoilT / countSoil;
        rec.soil_h     = accSoilH / countSoil;
      }

      populateDiagnostics(rec);
      resetAccumulators();

      uint32_t epochNow = getCurrentEpoch();
      if (epochNow >= MIN_VALID_EPOCH) {
        if (ramRingCount > 0) {
          flushRamRing(epochNow);
        }

        rec.timestamp = (alignedEpoch >= MIN_VALID_EPOCH) ? alignedEpoch : epochNow;
        lastSavedSlot = rec.timestamp / slotSec;

        if (saveRecordToSD(rec)) {
          if (rtcConsecutiveNetFailures == 0) {
            tryFlushNextFromSD();
            lastFlushMillis = millis();
          }
        } else {
          LOG_PRINTLN("[SD] Scrittura fallita: accodo in RAM e tento invio batch.");
          pushRecordToRamRing(rec);
          flushRamRingNetworkBatch();
          lastFlushMillis = millis();
        }
      } else {
        rec.timestamp = millis() / 1000UL;
        pushRecordToRamRing(rec);
        lastFlushMillis = millis();
      }
    }
  } else if (now - lastFlushMillis >= currentFlushIntervalMs) {
    tryFlushNextFromSD();
    lastFlushMillis = millis();
  }
}

void initHardwareWatchdog() {
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t wdtCfg = {
      .timeout_ms = WDT_TIMEOUT_SEC * 1000,
      .idle_core_mask = 0,
      .trigger_panic = true
    };
    esp_task_wdt_reconfigure(&wdtCfg);
  #else
    esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
  #endif
  esp_task_wdt_add(NULL);
}

void setup_main() {
  setCpuFrequencyMhz(ESP32_CPU_FREQ_MHZ);

  #if !DEBUG_LOG
    esp_log_level_set("*", ESP_LOG_NONE);
  #endif
  LOG_BEGIN(115200);
  delay(200);

  setenv("TZ", "UTC0", 1);
  tzset();

  esp_reset_reason_t reason = esp_reset_reason();
  rtcLastResetReason = (uint8_t)reason;

  if (reason == ESP_RST_POWERON || rtcStateMagic != RTC_STATE_MAGIC) {
    rtcStateMagic = RTC_STATE_MAGIC;
    rtcConsecutiveNetFailures = 0;
    rtcWdtReboots = 0;
    rtcLastRainRaw = 0;
    rtcFirstRainReading = true;
  } else if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT ||
             reason == ESP_RST_WDT || reason == ESP_RST_PANIC) {
    if (rtcWdtReboots < 250) rtcWdtReboots++;
    incrementNetFailuresSaturated();
    LOG_PRINTF("[BOOT] Ripristino da WDT/PANIC (Reason: %u, Count: %u, NetFail: %u)\n",
               rtcLastResetReason, rtcWdtReboots, rtcConsecutiveNetFailures);
  }
  updateFlushBackoffInterval(currentFlushIntervalMs);

  initHardwareWatchdog();

  Serial2.begin(9600, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);

  initStorageSD();
  powerOnModemHardware();
  configCC1101Main();

  uint32_t bootEpoch = getEpochFromModem();
  if (bootEpoch >= MIN_VALID_EPOCH) {
    setSystemEpoch(bootEpoch);
  }

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { otaUpdating = true; esp_task_wdt_reset(); });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { esp_task_wdt_reset(); });
  ArduinoOTA.onEnd([]() { otaUpdating = false; esp_task_wdt_reset(); });
  ArduinoOTA.onError([](ota_error_t) { otaUpdating = false; esp_task_wdt_reset(); });

  unsigned long now = millis();
  wifiState = WIFI_STATE_OFF;
  wifiTimerMillis = now - WIFI_SCAN_INTERVAL_MS;
  lastMeteoMillis = now;
  lastCcResetMillis = now;
  lastSaveMillis = now;
  lastFlushMillis = now;
}

void loop_main() {
  esp_task_wdt_reset();
  unsigned long now = millis();

  manageHotspotWifi(now);
  if (otaUpdating) {
    delay(1);
    return;
  }

  pollCC1101(now);
  pollRS485(now);
  handleSaveAndFlush(now);

  delay(2);
}
