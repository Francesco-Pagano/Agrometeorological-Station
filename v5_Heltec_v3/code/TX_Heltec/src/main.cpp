#include <Arduino.h>
#include <SPI.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <RadioLib.h>
#include <Wire.h>
#include <RTClib.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <math.h>
#include "secrets.h"
#include "protocol.h"

#define WDT_TIMEOUT 30
#define TX_NODE_ID 1
#define MIN_VALID_EPOCH 1700000000UL

#define FLAG_HISTORICAL 0x01

#define CC_SCK 5
#define CC_MISO 4
#define CC_MOSI 6
#define CC_CSN 7
#define CC_GDO0 2
#define CC_GDO2 3
#define LORA_SCK 9
#define LORA_MISO 11
#define LORA_MOSI 10
#define LORA_NSS 8
#define LORA_DIO1 14
#define LORA_RST 12
#define LORA_BUSY 13

#define I2C_SDA 41
#define I2C_SCL 42

#define RXD1 46
#define TXD1 45

#define WIFI_OFF_TIME_MS 60000UL
#define WIFI_CONNECT_TIMEOUT_MS 12000UL
#define WIFI_TX_POWER WIFI_POWER_15dBm

#define BACKLOG_FILE "/offline_v3.bin"
#define TEMP_FILE "/temp.bin"
#define BACKLOG_MAX_PERCENT 40

SPIClass loraSPI(HSPI);
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, loraSPI);
RTC_DS3231 rtc;
Preferences prefs;

HardwareSerial leafSerial(1);
const byte leafRequest[] = {0x01, 0x04, 0x00, 0x00, 0x00, 0x02, 0x71, 0xCB};

unsigned long previousMillisLeaf = 0;
const long intervalLeaf = 10000;
unsigned long previousMillisLoRa = 0;
const long intervalLoRa = 600000;
unsigned long previousMillisFlush = 0;
const long intervalFlush = 60000;

bool isFlushing = false;
size_t fileCursor = 0;
uint8_t flushCounter = 0;
uint16_t currentSeqNum = 0;

static const size_t NO_CURSOR = (size_t)-1;
size_t retryCursor = NO_CURSOR;
uint16_t retrySeq = 0;

bool rtcPresent = false;
bool rtcOk = false;
uint32_t softEpoch = 0;
uint32_t softMillis = 0;

unsigned long lastMeteoDataMillis = 0;
unsigned long lastCcResetMillis = 0;

uint16_t last_rain_raw = 0;
bool first_rain_reading = true;

float accTemp = 0, accHum = 0, accWind = 0, accSin = 0, accCos = 0, accGust = 0, accRain = 0;
uint16_t accN = 0;
int lastLowBat = -1;
float accLeafT = 0, accLeafW = 0;
uint16_t accLeafN = 0;

LoRaPacket txPacket;
volatile bool loraInterrupt = false;

#if defined(ESP8266) || defined(ESP32)
IRAM_ATTR
#endif
void setFlag(void) { loraInterrupt = true; }

static void wdtInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg;
  cfg.timeout_ms = WDT_TIMEOUT * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT, true);
#endif
  esp_task_wdt_add(NULL);
}

uint8_t crc8(uint8_t const message[], unsigned nBytes, uint8_t polynomial, uint8_t init) {
  uint8_t remainder = init;
  for (unsigned byte = 0; byte < nBytes; ++byte) {
    remainder ^= message[byte];
    for (unsigned bit = 0; bit < 8; ++bit) {
      remainder = (remainder & 0x80) ? (remainder << 1) ^ polynomial : (remainder << 1);
    }
  }
  return remainder;
}

uint16_t modbusCRC(uint8_t *buf, int len) {
  uint16_t crc = 0xFFFF;
  for (int pos = 0; pos < len; pos++) {
    crc ^= (uint16_t)buf[pos];
    for (int i = 8; i != 0; i--) {
      if ((crc & 0x0001) != 0) { crc >>= 1; crc ^= 0xA001; }
      else crc >>= 1;
    }
  }
  return crc;
}

uint32_t nowEpoch() {
  uint32_t t = 0;
  if (rtcOk) t = rtc.now().unixtime();
  else if (softEpoch) t = softEpoch + (millis() - softMillis) / 1000;
  return (t >= MIN_VALID_EPOCH) ? t : 0;
}

void syncClock(uint32_t epoch) {
  softEpoch = epoch;
  softMillis = millis();
  if (rtcPresent) {
    if (!rtcOk || labs((long)epoch - (long)rtc.now().unixtime()) > 2) rtc.adjust(DateTime(epoch));
    rtcOk = true;
  }
}

void markAllInvalid(SensorData& d) {
  d.timestamp = 0;
  d.temp = -999.0;
  d.hum = -999;
  d.wind_avg = -999.0;
  d.wind_gust = -999.0;
  d.wind_dir = -999;
  d.rain_mm = -999.0;
  d.lux = -999.0;
  d.uv_index = -999;
  d.leaf_temp = -999.0;
  d.leaf_wet = -999.0;
  d.low_battery = -1;
}

void decodeFineOffset(uint8_t* b) {
  uint8_t crc = crc8(&b[1], 15, 0x31, 0x00);
  uint8_t checksum = 0;
  for (int n = 1; n < 17; ++n) checksum += b[n];
  if (crc != b[16] || checksum != b[17]) return;

  float current_temp = (((b[4] & 0x07) << 8 | b[5]) - 400) * 0.1f;
  int current_hum = b[6];
  if (current_hum < 0 || current_hum > 100 || current_temp < -50 || current_temp > 80) return;

  int wind_dir = b[3] | (b[4] & 0x80) << 1;
  float current_wind = (b[7] | (b[4] & 0x10) << 4) * 0.125f * 0.51f;
  float current_gust = b[8] * 0.51f;

  accTemp += current_temp;
  accHum += current_hum;
  accWind += current_wind;
  if (wind_dir >= 0 && wind_dir <= 360) {
    float rad = wind_dir * DEG_TO_RAD;
    accSin += sinf(rad);
    accCos += cosf(rad);
  }
  if (current_gust > accGust) accGust = current_gust;
  lastLowBat = (b[4] & 0x08) >> 3;
  accN++;

  uint16_t current_rain_raw = (b[9] << 8 | b[10]);
  if (!first_rain_reading && current_rain_raw != last_rain_raw) {
    float delta = (current_rain_raw < last_rain_raw) ?
                  (65536 - last_rain_raw + current_rain_raw) * 0.254f :
                  (current_rain_raw - last_rain_raw) * 0.254f;
    if (delta < 100.0f) accRain += delta;
  }
  last_rain_raw = current_rain_raw;
  first_rain_reading = false;

  lastMeteoDataMillis = millis();
}

bool fillPacketFromAccumulators() {
  SensorData& d = txPacket.data;
  markAllInvalid(d);

  if (accN > 0) {
    d.temp = accTemp / accN;
    d.hum = (int)lroundf(accHum / accN);
    d.wind_avg = accWind / accN;
    d.wind_gust = accGust;
    float dir = atan2f(accSin, accCos) * RAD_TO_DEG;
    if (dir < 0) dir += 360.0f;
    d.wind_dir = ((int)lroundf(dir)) % 360;
    d.rain_mm = accRain;
    d.low_battery = lastLowBat;
  }
  if (accLeafN > 0) {
    d.leaf_temp = accLeafT / accLeafN;
    d.leaf_wet = accLeafW / accLeafN;
  }

  bool hasData = (accN > 0) || (accLeafN > 0);

  accTemp = accHum = accWind = accSin = accCos = accGust = accRain = 0;
  accN = 0;
  accLeafT = accLeafW = 0;
  accLeafN = 0;
  return hasData;
}

static size_t fileSizeOf(const char* path) {
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return 0;
  size_t s = f.size();
  f.close();
  return s;
}

static bool rewriteFile(const char* path, size_t from, size_t len) {
  if (LittleFS.totalBytes() - LittleFS.usedBytes() < len + 8192) return false;

  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  File t = LittleFS.open(TEMP_FILE, FILE_WRITE);
  if (!t) { f.close(); return false; }

  bool ok = f.seek(from);
  size_t written = 0;
  uint8_t buf[256];
  while (ok && written < len) {
    esp_task_wdt_reset();
    size_t toRead = min((size_t)sizeof(buf), len - written);
    size_t r = f.read(buf, toRead);
    if (r == 0) break;
    if (t.write(buf, r) != r) { ok = false; break; }
    written += r;
  }
  t.close();
  f.close();

  if (!ok || written != len) {
    LittleFS.remove(TEMP_FILE);
    return false;
  }
  LittleFS.remove(path);
  LittleFS.rename(TEMP_FILE, path);
  return true;
}

static void recoverBacklog(const char* path) {
  if (!LittleFS.exists(TEMP_FILE)) return;
  if (!LittleFS.exists(path)) LittleFS.rename(TEMP_FILE, path);
  else LittleFS.remove(TEMP_FILE);
}

static size_t backlogMaxBytes(size_t recSize) {
  size_t cap = (LittleFS.totalBytes() / 100) * BACKLOG_MAX_PERCENT;
  return (cap / recSize) * recSize;
}

static bool compactBacklog(const char* path, size_t recSize, size_t& cursor, Preferences& prf) {
  size_t fSize = fileSizeOf(path);
  if (fSize == 0) return false;

  size_t keep = (min(cursor, fSize) / recSize) * recSize;
  size_t remaining = fSize - keep;
  if (remaining > backlogMaxBytes(recSize) / 2) keep += ((remaining / 2) / recSize) * recSize;
  if (keep == 0) keep = min(recSize, fSize);

  if (keep >= fSize) {
    prf.putUInt("cursor", 0);
    File f = LittleFS.open(path, FILE_WRITE);
    if (f) f.close();
    cursor = 0;
    return true;
  }

  size_t newCursor = (cursor > keep) ? cursor - keep : 0;
  prf.putUInt("cursor", newCursor);
  if (!rewriteFile(path, keep, fSize - keep)) {
    prf.putUInt("cursor", cursor);
    return false;
  }
  cursor = newCursor;
  return true;
}

static bool appendBacklog(const char* path, const void* rec, size_t recSize, size_t& cursor, Preferences& prf) {
  size_t curSize = fileSizeOf(path);

  if (curSize % recSize != 0) {
    size_t aligned = (curSize / recSize) * recSize;
    if (!rewriteFile(path, 0, aligned)) return false;
    if (cursor > aligned) { cursor = aligned; prf.putUInt("cursor", cursor); }
    curSize = aligned;
  }

  size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (curSize + recSize > backlogMaxBytes(recSize) || freeBytes < 8192) {
    if (!compactBacklog(path, recSize, cursor, prf)) return false;
  }

  File f = LittleFS.open(path, FILE_APPEND);
  if (!f) return false;
  size_t w = f.write((const uint8_t*)rec, recSize);
  f.close();
  return w == recSize;
}

static bool initRadio() {
  if (radio.begin(868.5, 125.0, 9, 7, 0x12, 14) != RADIOLIB_ERR_NONE) return false;
  radio.setPacketReceivedAction(setFlag);
  return true;
}

bool sendAndWaitAck(LoRaPacket& pkt) {
  ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
  bool success = false;

  if (radio.transmit((uint8_t*)&pkt, sizeof(pkt)) == RADIOLIB_ERR_NONE) {
    loraInterrupt = false;
    radio.startReceive();
    unsigned long waitStart = millis();
    while ((int32_t)(millis() - waitStart) < 2500) {
      if (loraInterrupt) {
        loraInterrupt = false;
        AckPacket ack;
        size_t plen = radio.getPacketLength();
        if (radio.readData((uint8_t*)&ack, sizeof(ack)) == RADIOLIB_ERR_NONE) {
          if (plen == sizeof(AckPacket) && ack.magic == PROTOCOL_MAGIC && ack.seq_num == pkt.header.seq_num) {
            success = true;
            if (ack.ntp_epoch > MIN_VALID_EPOCH) syncClock(ack.ntp_epoch);
            break;
          }
        }
        radio.startReceive();
      }
      delay(5);
    }
    radio.standby();
  }
  ELECHOUSE_cc1101.SpiStrobe(CC1101_SFRX);
  ELECHOUSE_cc1101.SetRx();
  return success;
}

void configCC1101() {
  ELECHOUSE_cc1101.setSpiPin(CC_SCK, CC_MISO, CC_MOSI, CC_CSN);
  ELECHOUSE_cc1101.setGDO(CC_GDO0, CC_GDO2);
  ELECHOUSE_cc1101.Init();
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
}

enum WsState : uint8_t { WS_OFF, WS_CONNECTING, WS_UP };
WsState wsState = WS_OFF;
unsigned long wsMillis = 0;
bool otaStarted = false;

static void wifiGoOff(unsigned long now) {
  if (otaStarted) { ArduinoOTA.end(); otaStarted = false; }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wsState = WS_OFF;
  wsMillis = now;
}

static void manageWifi(unsigned long now) {
  switch (wsState) {
    case WS_OFF:
      if ((int32_t)(now - wsMillis) >= (int32_t)WIFI_OFF_TIME_MS) {
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(false);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        WiFi.setTxPower(WIFI_TX_POWER);
        wsState = WS_CONNECTING;
        wsMillis = now;
      }
      break;

    case WS_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        ArduinoOTA.begin();
        otaStarted = true;
        wsState = WS_UP;
        wsMillis = now;
      } else if ((int32_t)(now - wsMillis) >= (int32_t)WIFI_CONNECT_TIMEOUT_MS) {
        wifiGoOff(now);
      }
      break;

    case WS_UP:
      if (WiFi.status() != WL_CONNECTED) wifiGoOff(now);
      else ArduinoOTA.handle();
      break;
  }
}

void setup() {
  setCpuFrequencyMhz(80);
  Serial.begin(115200);
  pinMode(LORA_DIO1, INPUT);

  wdtInit();
  WiFi.mode(WIFI_OFF);

  Wire.begin(I2C_SDA, I2C_SCL);
  rtcPresent = rtc.begin(&Wire);
  rtcOk = rtcPresent && !rtc.lostPower();
  if (rtcOk && rtc.now().unixtime() < MIN_VALID_EPOCH) rtcOk = false;

  LittleFS.begin(true);
  LittleFS.remove("/offline.bin");
  LittleFS.remove("/offline_v2.bin");
  recoverBacklog(BACKLOG_FILE);

  prefs.begin("tx_store", false);
  fileCursor = prefs.getUInt("cursor", 0);

  const size_t rec = sizeof(SensorData);
  size_t fSize = fileSizeOf(BACKLOG_FILE);
  if (fileCursor > fSize) { fileCursor = 0; prefs.putUInt("cursor", 0); }
  if (fSize % rec != 0) {
    size_t safeSize = (fSize / rec) * rec;
    rewriteFile(BACKLOG_FILE, 0, safeSize);
    if (fileCursor > safeSize) { fileCursor = 0; prefs.putUInt("cursor", 0); }
  }
  if (fileCursor % rec != 0) {
    fileCursor = (fileCursor / rec) * rec;
    prefs.putUInt("cursor", fileCursor);
  }

  loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  uint8_t loraRetries = 0;
  while (!initRadio() && loraRetries < 3) {
    esp_task_wdt_reset();
    delay(500);
    loraRetries++;
  }
  if (loraRetries >= 3) ESP.restart();

  configCC1101();

  leafSerial.begin(9600, SERIAL_8N1, RXD1, TXD1);

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { esp_task_wdt_delete(NULL); });
  ArduinoOTA.onError([](ota_error_t error) { esp_task_wdt_add(NULL); });

  wsState = WS_OFF;
  wsMillis = millis() - WIFI_OFF_TIME_MS;

  currentSeqNum = esp_random() & 0xFFFF;

  memset(&txPacket, 0, sizeof(txPacket));
  txPacket.header.magic = PROTOCOL_MAGIC;
  txPacket.header.version = PROTOCOL_VERSION;
  txPacket.header.node_id = TX_NODE_ID;
  markAllInvalid(txPacket.data);

  unsigned long now = millis();
  lastMeteoDataMillis = now;
  lastCcResetMillis = now;

  previousMillisLoRa = now - intervalLoRa + 75000;
}


static void handleCC1101(unsigned long now) {
  if ((int32_t)(now - lastCcResetMillis) > 300000) {
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SFRX);
    ELECHOUSE_cc1101.SetRx();
    lastCcResetMillis = now;
  }

  if ((int32_t)(now - lastMeteoDataMillis) > 1200000) {
    configCC1101();
    lastMeteoDataMillis = now;
  }

  uint8_t rxb = ELECHOUSE_cc1101.SpiReadStatus(CC1101_RXBYTES);
  if (rxb & 0x80) {
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SFRX);
    ELECHOUSE_cc1101.SetRx();
  } else if ((rxb & 0x7F) >= 27) {
    uint8_t buf[27];
    ELECHOUSE_cc1101.SpiReadBurstReg(CC1101_RXFIFO, buf, 27);
    if (buf[0] == 0xD4 || buf[0] == 0x24) decodeFineOffset(buf);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SFRX);
    ELECHOUSE_cc1101.SetRx();
  }
}

static void handleLeaf(unsigned long now) {
  if ((int32_t)(now - previousMillisLeaf) < intervalLeaf) return;
  previousMillisLeaf = now;

  while (leafSerial.available()) leafSerial.read();
  leafSerial.write(leafRequest, sizeof(leafRequest));
  leafSerial.flush();

  unsigned long waitStart = millis();
  while (leafSerial.available() < 9 && (int32_t)(millis() - waitStart) < 100) { delay(2); }

  if (leafSerial.available() >= 9) {
    uint8_t res[9];
    for (int i = 0; i < 9; i++) res[i] = leafSerial.read();
    uint16_t calcCRC = modbusCRC(res, 7);
    uint16_t recCRC = (res[8] << 8) | res[7];

    if (res[0] == 0x01 && res[1] == 0x04 && res[2] == 0x04 && calcCRC == recCRC) {
      accLeafT += ((int16_t)(res[3] << 8 | res[4])) / 100.0f;
      accLeafW += ((uint16_t)(res[5] << 8 | res[6])) / 100.0f;
      accLeafN++;
    }
  }
}

static void handleLoraLive(unsigned long now) {
  if ((int32_t)(now - previousMillisLoRa) < intervalLoRa) return;
  previousMillisLoRa = now;

  bool hasData = fillPacketFromAccumulators();

  txPacket.header.seq_num = currentSeqNum++;
  txPacket.header.flags = 0;
  txPacket.data.timestamp = nowEpoch();

  if (sendAndWaitAck(txPacket)) {
    size_t fSize = fileSizeOf(BACKLOG_FILE);
    if (fileCursor + sizeof(SensorData) <= fSize) isFlushing = true;
  } else {
    isFlushing = false;

    if (hasData) {
      if (!appendBacklog(BACKLOG_FILE, &txPacket.data, sizeof(SensorData), fileCursor, prefs)) {
        Serial.println("TX: backlog non scrivibile, record perso");
      }
      retryCursor = NO_CURSOR;
    }
  }
}

static void handleLoraFlush(unsigned long now) {
  if (!isFlushing || (int32_t)(now - previousMillisFlush) < intervalFlush) return;
  previousMillisFlush = now;

  const size_t rec = sizeof(SensorData);
  size_t fSize = fileSizeOf(BACKLOG_FILE);
  if (fSize % rec != 0) fileCursor = (fileCursor / rec) * rec;

  if (fileCursor + rec <= fSize) {
    SensorData histData;
    size_t bytesRead = 0;
    File file = LittleFS.open(BACKLOG_FILE, FILE_READ);
    if (file) {
      file.seek(fileCursor);
      bytesRead = file.read((uint8_t*)&histData, rec);
      file.close();
    }

    if (bytesRead == rec) {
      LoRaPacket histPkt;
      memset(&histPkt, 0, sizeof(histPkt));
      histPkt.header.magic = PROTOCOL_MAGIC;
      histPkt.header.version = PROTOCOL_VERSION;
      histPkt.header.node_id = TX_NODE_ID;
      uint16_t seq = (retryCursor == fileCursor) ? retrySeq : currentSeqNum++;
      histPkt.header.seq_num = seq;
      histPkt.header.flags = FLAG_HISTORICAL;
      histPkt.data = histData;

      if (sendAndWaitAck(histPkt)) {
        retryCursor = NO_CURSOR;
        fileCursor += rec;
        if (++flushCounter >= 10) { prefs.putUInt("cursor", fileCursor); flushCounter = 0; }
      } else {
        retryCursor = fileCursor;
        retrySeq = seq;
        isFlushing = false;
      }
    } else {
      fileCursor = fSize;
    }
    return;
  }

  if (fSize > 0) {
    File f = LittleFS.open(BACKLOG_FILE, FILE_WRITE);
    if (f) f.close();
  }
  fileCursor = 0;
  prefs.putUInt("cursor", 0);
  flushCounter = 0;
  retryCursor = NO_CURSOR;
  isFlushing = false;
}

void loop() {
  esp_task_wdt_reset();
  unsigned long now = millis();

  manageWifi(now);
  handleCC1101(now);
  handleLeaf(now);
  handleLoraLive(now);
  handleLoraFlush(now);

  delay(10);
}