#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include <stdarg.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include "secrets.h"
#include "protocol.h"

#ifdef OTA_ROOT_CA
#include <WiFiClientSecure.h>
typedef WiFiClientSecure OtaClient;
#else
typedef WiFiClient OtaClient;
#endif

#define WDT_TIMEOUT 30
const String CURRENT_VERSION = FW_VERSION;

#define LORA_SCK 9
#define LORA_MISO 11
#define LORA_MOSI 10
#define LORA_NSS 8
#define LORA_DIO1 14
#define LORA_RST 12
#define LORA_BUSY 13

#define MIN_VALID_EPOCH 1700000000UL

#define FLAG_HISTORICAL   0x01
#define FLAG_TS_ESTIMATED 0x02

#define BACKLOG_FILE "/rx_backlog_v3.bin"
#define TEMP_FILE "/temp.bin"
#define BACKLOG_MAX_PERCENT 40
#define RX_FLUSH_INTERVAL_MS 1000

SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);
SemaphoreHandle_t radioMutex;

WiFiClient espClient;
PubSubClient client(espClient);
Preferences rxPrefs;

QueueHandle_t dataQueue;
volatile bool loraInterrupt = false;
volatile bool suspendRadio = false;

struct MqttMessage {
  LoRaPacket pkt;
  float rssi;
  float snr;
};

#define DEDUP_SIZE 32
uint32_t recentTimes[DEDUP_SIZE] = {0};
uint16_t recentSeqs[DEDUP_SIZE] = {0};
uint8_t recentNodes[DEDUP_SIZE] = {0};
uint8_t dedupIndex = 0;

unsigned long lastReconnectAttempt = 0;
unsigned long previousMillisOTA = 0;
const long intervalOTA = 3600000;

bool rxIsFlushing = false;
unsigned long previousMillisRXFlush = 0;
size_t rxFileCursor = 0;
uint8_t rxFlushSaved = 0;

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

bool isDuplicate(uint8_t node, uint32_t ts, uint16_t seq) {
  for (int i = 0; i < DEDUP_SIZE; i++) {
    if (recentNodes[i] == node) {
      if (ts > MIN_VALID_EPOCH && recentTimes[i] == ts) return true;
      if (ts <= MIN_VALID_EPOCH && recentSeqs[i] == seq) return true;
    }
  }
  return false;
}

void addDedup(uint8_t node, uint32_t ts, uint16_t seq) {
  recentNodes[dedupIndex] = node;
  recentTimes[dedupIndex] = ts;
  recentSeqs[dedupIndex] = seq;
  dedupIndex = (dedupIndex + 1) % DEDUP_SIZE;
}

static void jAdd(char* buf, size_t cap, size_t& pos, const char* fmt, ...) {
  if (pos >= cap - 1) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + pos, cap - pos, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n >= cap - pos) pos = cap - 1;
  else pos += (size_t)n;
}

void buildJson(char* buffer, size_t maxLen, SensorData& data, uint8_t node_id,
               float rssi, float snr, bool isHist, bool tsEstimated) {
  size_t pos = 0;
  buffer[0] = '\0';
  jAdd(buffer, maxLen, pos, "{\"node_id\":%d,", (int)node_id);

  if (data.timestamp >= MIN_VALID_EPOCH) jAdd(buffer, maxLen, pos, "\"timestamp\":%lu,", (unsigned long)data.timestamp);

  if (data.temp > -900)      jAdd(buffer, maxLen, pos, "\"temp\":%.1f,", (double)data.temp);
  if (data.hum > -900)       jAdd(buffer, maxLen, pos, "\"hum\":%d,", (int)data.hum);
  if (data.wind_avg > -900)  jAdd(buffer, maxLen, pos, "\"wind_avg\":%.1f,", (double)data.wind_avg);
  if (data.wind_gust > -900) jAdd(buffer, maxLen, pos, "\"wind_gust\":%.1f,", (double)data.wind_gust);
  if (data.wind_dir > -900)  jAdd(buffer, maxLen, pos, "\"wind_dir\":%d,", (int)data.wind_dir);
  if (data.rain_mm > -900)   jAdd(buffer, maxLen, pos, "\"rain_mm\":%.1f,", (double)data.rain_mm);
  if (data.lux > -900)       jAdd(buffer, maxLen, pos, "\"lux\":%.1f,", (double)data.lux);
  if (data.uv_index > -900)  jAdd(buffer, maxLen, pos, "\"uv_index\":%d,", (int)data.uv_index);
  if (data.leaf_temp > -900) jAdd(buffer, maxLen, pos, "\"leaf_temp\":%.1f,", (double)data.leaf_temp);
  if (data.leaf_wet > -900)  jAdd(buffer, maxLen, pos, "\"leaf_wet\":%.1f,", (double)data.leaf_wet);
  if (data.low_battery != -1) jAdd(buffer, maxLen, pos, "\"tx_lowbat\":%d,", (int)data.low_battery);

  if (!isHist) {
    jAdd(buffer, maxLen, pos, "\"rssi\":%.1f,", (double)rssi);
    jAdd(buffer, maxLen, pos, "\"snr\":%.1f,", (double)snr);
  }
  if (tsEstimated) jAdd(buffer, maxLen, pos, "\"ts_estimated\":1,");

  jAdd(buffer, maxLen, pos, "\"is_historical\":%d,\"uptime\":%lu}", isHist ? 1 : 0, (unsigned long)(millis() / 1000));
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

static void fixTimestamp(LoRaPacket& pkt, bool late) {
  if (pkt.data.timestamp >= MIN_VALID_EPOCH) return;
  time_t t = time(nullptr);
  if (t < (time_t)MIN_VALID_EPOCH) return;
  pkt.data.timestamp = (uint32_t)t;
  if (late || (pkt.header.flags & FLAG_HISTORICAL)) pkt.header.flags |= FLAG_TS_ESTIMATED;
}

static bool parseVersion(const String& s, int v[3]) {
  v[0] = v[1] = v[2] = 0;
  const char* p = s.c_str();
  if (*p == 'v' || *p == 'V') p++;
  return sscanf(p, "%d.%d.%d", &v[0], &v[1], &v[2]) >= 1;
}

static bool isNewerVersion(const String& server, const String& current) {
  int a[3], b[3];
  if (!parseVersion(server, a) || !parseVersion(current, b)) return server != current;
  for (int i = 0; i < 3; i++) {
    if (a[i] != b[i]) return a[i] > b[i];
  }
  return false;
}

void checkOTAUpdate() {
  if (WiFi.status() != WL_CONNECTED) return;
  String serverVersion = "";

  {
    OtaClient checkClient;
#ifdef OTA_ROOT_CA
    checkClient.setCACert(OTA_ROOT_CA);
#endif
    HTTPClient http;
    http.setTimeout(8000);
    if (http.begin(checkClient, OTA_VERSION_URL) && http.GET() == HTTP_CODE_OK) {
      serverVersion = http.getString();
      serverVersion.trim();
    }
    http.end();
  }

  if (serverVersion.length() == 0 || serverVersion.length() > 32) return;
  if (!isNewerVersion(serverVersion, CURRENT_VERSION)) return;

  if (rxPrefs.getString("ota_try", "") == serverVersion) return;
  rxPrefs.putString("ota_try", serverVersion);

  suspendRadio = true;
  delay(100);

  if (xSemaphoreTake(radioMutex, portMAX_DELAY)) {
    radio.standby();
    xSemaphoreGive(radioMutex);
  }

  if (client.connected()) client.disconnect();

  esp_task_wdt_delete(NULL);
  {
    OtaClient updateClient;
#ifdef OTA_ROOT_CA
    updateClient.setCACert(OTA_ROOT_CA);
#endif
    updateClient.setTimeout(10000);
    httpUpdate.update(updateClient, OTA_FIRMWARE_URL);
  }

  rxPrefs.remove("ota_try");
  esp_task_wdt_add(NULL);
  suspendRadio = false;
  if (xSemaphoreTake(radioMutex, portMAX_DELAY)) {
    radio.startReceive();
    xSemaphoreGive(radioMutex);
  }
}

static bool initRadio() {
  if (radio.begin(868.5, 125.0, 9, 7, 0x12, 14) != RADIOLIB_ERR_NONE) return false;
  radio.setPacketReceivedAction(setFlag);
  return radio.startReceive() == RADIOLIB_ERR_NONE;
}

static void sendAck(uint16_t seq) {
  time_t now = time(nullptr);
  uint32_t epoch = (now > (time_t)MIN_VALID_EPOCH) ? (uint32_t)now : 0;
  AckPacket ack = {PROTOCOL_MAGIC, seq, epoch};
  vTaskDelay(pdMS_TO_TICKS(30));
  radio.transmit((uint8_t*)&ack, sizeof(ack));
  loraInterrupt = false;
}

void loraTask(void *pvParameters) {
  esp_task_wdt_add(NULL);
  unsigned long lastRxMillis = millis();

  while (1) {
    esp_task_wdt_reset();

    if (suspendRadio) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

    if (loraInterrupt) {
      loraInterrupt = false;
      if (xSemaphoreTake(radioMutex, portMAX_DELAY)) {
        LoRaPacket rcvPkt;
        size_t plen = radio.getPacketLength();
        int state = radio.readData((uint8_t*)&rcvPkt, sizeof(rcvPkt));
        float cRssi = radio.getRSSI();
        float cSnr = radio.getSNR();

        if (state == RADIOLIB_ERR_NONE && plen == sizeof(LoRaPacket) &&
            rcvPkt.header.magic == PROTOCOL_MAGIC && rcvPkt.header.version == PROTOCOL_VERSION) {

          lastRxMillis = millis();

          if (!isDuplicate(rcvPkt.header.node_id, rcvPkt.data.timestamp, rcvPkt.header.seq_num)) {
            MqttMessage msg = {rcvPkt, cRssi, cSnr};
            if (xQueueSend(dataQueue, &msg, pdMS_TO_TICKS(50)) == pdPASS) {
              addDedup(rcvPkt.header.node_id, rcvPkt.data.timestamp, rcvPkt.header.seq_num);
              sendAck(rcvPkt.header.seq_num);
            }
          } else {
            sendAck(rcvPkt.header.seq_num);
          }
        }
        radio.startReceive();
        xSemaphoreGive(radioMutex);
      }
    }

    if ((int32_t)(millis() - lastRxMillis) > 1800000) {
      if (xSemaphoreTake(radioMutex, portMAX_DELAY)) {
        bool ok = initRadio();
        xSemaphoreGive(radioMutex);
        lastRxMillis = ok ? millis() : millis() - 1790000;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LORA_DIO1, INPUT);

  radioMutex = xSemaphoreCreateMutex();
  wdtInit();

  LittleFS.begin(true);
  LittleFS.remove("/rx_backlog.bin");
  LittleFS.remove("/rx_backlog_v2.bin");
  recoverBacklog(BACKLOG_FILE);

  rxPrefs.begin("rx_store", false);
  rxFileCursor = rxPrefs.getUInt("cursor", 0);

  const size_t rec = sizeof(LoRaPacket);
  size_t fSize = fileSizeOf(BACKLOG_FILE);
  if (rxFileCursor > fSize) { rxFileCursor = 0; rxPrefs.putUInt("cursor", 0); }
  if (fSize % rec != 0) {
    size_t safeSize = (fSize / rec) * rec;
    rewriteFile(BACKLOG_FILE, 0, safeSize);
    if (rxFileCursor > safeSize) { rxFileCursor = 0; rxPrefs.putUInt("cursor", 0); }
  }
  if (rxFileCursor % rec != 0) {
    rxFileCursor = (rxFileCursor / rec) * rec;
    rxPrefs.putUInt("cursor", rxFileCursor);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setBufferSize(768);
  client.setKeepAlive(10);
  client.setSocketTimeout(5);

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  uint8_t tries = 0;
  while (!initRadio()) {
    esp_task_wdt_reset();
    delay(500);
    if (++tries >= 20) ESP.restart();
  }

  dataQueue = xQueueCreate(10, sizeof(MqttMessage));
  xTaskCreatePinnedToCore(loraTask, "LoRaTask", 6144, NULL, 2, NULL, 0);
}

static void handleNetwork(unsigned long now) {
  static bool timeConfigured = false;
  static bool otaInit = false;
  static unsigned long lastWifiOk = 0;
  static unsigned long lastWifiKick = 0;

  if (WiFi.status() == WL_CONNECTED) {
    lastWifiOk = now;

    if (!timeConfigured) {
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      timeConfigured = true;
    }

    if (!otaInit) {
      ArduinoOTA.setHostname(OTA_HOSTNAME);
      ArduinoOTA.setPassword(OTA_PASSWORD);
      ArduinoOTA.onStart([]() { suspendRadio = true; delay(100); esp_task_wdt_delete(NULL); });
      ArduinoOTA.onError([](ota_error_t error) { esp_task_wdt_add(NULL); suspendRadio = false; });
      ArduinoOTA.begin();
      otaInit = true;
    }
    ArduinoOTA.handle();

    if (!client.connected()) {
      if ((int32_t)(now - lastReconnectAttempt) > 5000) {
        lastReconnectAttempt = now;
        if (client.connect(mqtt_clientName, "status/rx", 1, true, "offline")) {
          client.publish("status/rx", "online", true);
          if (fileSizeOf(BACKLOG_FILE) >= rxFileCursor + sizeof(LoRaPacket)) rxIsFlushing = true;
        }
      }
    } else {
      client.loop();
    }
  } else {
    if ((int32_t)(now - lastWifiOk) > 30000 && (int32_t)(now - lastWifiKick) > 30000) {
      lastWifiKick = now;
      WiFi.disconnect(false);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    if ((int32_t)(now - lastWifiOk) > 1800000) ESP.restart();
  }
}

static void handleQueue() {
  MqttMessage qMsg;
  if (xQueueReceive(dataQueue, &qMsg, 0) != pdPASS) return;

  fixTimestamp(qMsg.pkt, false);
  bool isHist = (qMsg.pkt.header.flags & FLAG_HISTORICAL) != 0;
  bool tsEst = (qMsg.pkt.header.flags & FLAG_TS_ESTIMATED) != 0;

  char jsonBuffer[512];
  buildJson(jsonBuffer, sizeof(jsonBuffer), qMsg.pkt.data, qMsg.pkt.header.node_id,
            qMsg.rssi, qMsg.snr, isHist, tsEst);

  bool published = client.connected() && client.publish(mqtt_topic, jsonBuffer);

  if (!published) {
    if (appendBacklog(BACKLOG_FILE, &qMsg.pkt, sizeof(LoRaPacket), rxFileCursor, rxPrefs)) {
      rxIsFlushing = true;
    } else {
      Serial.println("RX: backlog non scrivibile, record perso");
    }
  }
}

static void handleFlush(unsigned long now) {
  if (!client.connected()) return;

  const size_t rec = sizeof(LoRaPacket);

  if (!rxIsFlushing) {
    static unsigned long lastCheck = 0;
    if ((int32_t)(now - lastCheck) > 30000) {
      lastCheck = now;
      if (fileSizeOf(BACKLOG_FILE) >= rxFileCursor + rec) rxIsFlushing = true;
    }
    return;
  }

  if ((int32_t)(now - previousMillisRXFlush) < RX_FLUSH_INTERVAL_MS) return;
  if (time(nullptr) < (time_t)MIN_VALID_EPOCH && now < 120000UL) return;
  previousMillisRXFlush = now;

  size_t fSize = fileSizeOf(BACKLOG_FILE);
  if (fSize % rec != 0) rxFileCursor = (rxFileCursor / rec) * rec;

  if (rxFileCursor + rec <= fSize) {
    LoRaPacket histPkt;
    size_t bytesRead = 0;
    File file = LittleFS.open(BACKLOG_FILE, FILE_READ);
    if (file) {
      file.seek(rxFileCursor);
      bytesRead = file.read((uint8_t*)&histPkt, rec);
      file.close();
    }

    if (bytesRead == rec) {
      fixTimestamp(histPkt, true);
      char jsonBuffer[512];
      buildJson(jsonBuffer, sizeof(jsonBuffer), histPkt.data, histPkt.header.node_id, 0, 0, true,
                (histPkt.header.flags & FLAG_TS_ESTIMATED) != 0);

      if (client.publish(mqtt_topic, jsonBuffer)) {
        rxFileCursor += rec;
        if (++rxFlushSaved >= 10) { rxPrefs.putUInt("cursor", rxFileCursor); rxFlushSaved = 0; }
      } else {
        rxIsFlushing = false;
      }
    } else {
      rxFileCursor = fSize;
    }
    return;
  }

  if (fSize > 0) {
    File f = LittleFS.open(BACKLOG_FILE, FILE_WRITE);
    if (f) f.close();
  }
  rxFileCursor = 0;
  rxPrefs.putUInt("cursor", 0);
  rxFlushSaved = 0;
  rxIsFlushing = false;
}

void loop() {
  esp_task_wdt_reset();
  unsigned long now = millis();

  handleNetwork(now);

  if ((int32_t)(now - previousMillisOTA) >= intervalOTA) {
    previousMillisOTA = now;
    checkOTAUpdate();
  }

  handleQueue();
  handleFlush(now);
  delay(1);
}