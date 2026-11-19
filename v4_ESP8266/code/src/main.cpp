#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
extern "C" {
  #include <user_interface.h>
}

#include "config.h"
#include "storage_queue.h"
#include "mqtt_qos1.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// RTC CLOCK
struct RtcClockStore {
  uint32_t magic;
  uint32_t epoch;
  uint32_t crc;
};
static const uint32_t RTC_CLOCK_MAGIC = 0x54494D45UL; // "TIME"

static const uint8_t LOG_LINES    = 28;
static const uint8_t LOG_LINE_LEN = 88;
static char          logRing[LOG_LINES][LOG_LINE_LEN];
static uint8_t       logHead  = 0;
static uint8_t       logCount = 0;

void logMsg(const char* fmt, ...) {
  char buf[LOG_LINE_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

#if DEBUG
  Serial.println(buf);
#endif

  strncpy(logRing[logHead], buf, LOG_LINE_LEN - 1);
  logRing[logHead][LOG_LINE_LEN - 1] = '\0';
  logHead = (uint8_t)((logHead + 1) % LOG_LINES);
  if (logCount < LOG_LINES) logCount++;
}

uint32_t getUptimeSec() {
  static uint32_t lastMs  = 0;
  static uint64_t totalMs = 0;
  uint32_t cur = millis();
  totalMs += (uint32_t)(cur - lastMs);
  lastMs   = cur;
  return (uint32_t)(totalMs / 1000ULL);
}

Adafruit_BME680  bme;
ESP8266WebServer debugServer(80);
StorageQueue     queue;
MqttQos1Client   mqtt;

volatile unsigned long rainClicks        = 0;
volatile unsigned long lastRainTime      = 0;
volatile unsigned long windClicks        = 0;
volatile unsigned long gustClicks        = 0;
volatile unsigned long lastWindClickTime = 0;

void IRAM_ATTR handleRainInterrupt() {
  unsigned long now = millis();
  if (now - lastRainTime > RAIN_DEBOUNCE_MS) {
    rainClicks++;
    lastRainTime = now;
  }
}

void IRAM_ATTR handleWindSpeedInterrupt() {
  unsigned long now = millis();
  if (now - lastWindClickTime > WIND_DEBOUNCE_MS) {
    windClicks++;
    gustClicks++;
    lastWindClickTime = now;
  }
}

// ACCUMULATORI
bool          bmeFound           = false;
bool          otaUpdating        = false;
uint32_t      currentBootId      = 0;
uint32_t      lastAlignedSlot    = 0;

double        sumTemp            = 0.0;
double        sumHum             = 0.0;
double        sumPres            = 0.0;
double        sumGas             = 0.0;
uint16_t      bmeSampleCount     = 0;

double        sumWindSin         = 0.0;
double        sumWindCos         = 0.0;
uint16_t      windDirSamples     = 0;
int           lastValidWindDir   = 0;
float         lastGustKmh        = 0.0f;
float         maxGustKmh         = 0.0f;

unsigned long lastSendTime       = 0;
unsigned long lastGustCalcTime   = 0;
unsigned long lastWindDirTime    = 0;
unsigned long lastBmeSampleTime  = 0;
unsigned long lastBmeRetryTime   = 0;
unsigned long lastReconcileTime  = 0;
unsigned long lastWifiRetryTime  = 0;
unsigned long wifiLostAtMs       = 0;
unsigned long wifiHardResetAtMs  = 0;

// SENSORI
bool initBME680() {
  if (!bme.begin(0x76)) {
    if (!bme.begin(0x77)) {
      bmeFound = false;
      return false;
    }
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);
  bmeFound = true;
  return true;
}

void sampleBME680() {
  if (!bmeFound) return;
  if (!bme.performReading()) {
    logMsg("[BME680] Lettura fallita, campione ignorato.");
    return;
  }

  float t = bme.temperature;
  float h = bme.humidity;
  float p = bme.pressure / 100.0f;
  float g = bme.gas_resistance / 1000.0f;

  if (!isfinite(t) || !isfinite(h) || !isfinite(p) || !isfinite(g) ||
      t < -40.0f || t > 85.0f || h < 0.0f || h > 100.0f || p < 800.0f || p > 1100.0f) {
    logMsg("[BME680] Valori fuori range fisico, scartati.");
    return;
  }

  sumTemp += t;
  sumHum  += h;
  sumPres += p;
  sumGas  += g;
  bmeSampleCount++;
}

void sampleWindDirection() {
  int adc  = analogRead(WIND_DIR_PIN);
  int wDir = getWindDirectionDegrees(adc);
  if (wDir < 0) return;

  lastValidWindDir = wDir;
  double rad = (double)wDir * (M_PI / 180.0);
  double w   = (lastGustKmh > 0.5f) ? (double)lastGustKmh : 0.2;
  sumWindSin += sin(rad) * w;
  sumWindCos += cos(rad) * w;
  windDirSamples++;
}

void buildWindowRecord(WeatherRecord& rec, unsigned long currentMillis) {
  noInterrupts();
  unsigned long cWind = windClicks;
  unsigned long cRain = rainClicks;
  windClicks = 0;
  rainClicks = 0;
  interrupts();

  if (bmeSampleCount == 0 && bmeFound) {
    sampleBME680();
  }

  float elapsedSec = (currentMillis - lastSendTime) / 1000.0f;
  float rps        = (elapsedSec > 1.0f) ? ((float)cWind / elapsedSec) : 0.0f;
  float wSpeed     = rps * WIND_SPEED_FACTOR;
  float rainMM     = (float)cRain * BUCKET_SIZE;

  int avgDir = lastValidWindDir;
  if (windDirSamples > 0 && (fabs(sumWindSin) > 1e-6 || fabs(sumWindCos) > 1e-6)) {
    double deg = atan2(sumWindSin, sumWindCos) * (180.0 / M_PI);
    if (deg < 0.0) deg += 360.0;
    avgDir = (int)(lround(deg) % 360L);
  }

  rec = {};
  time_t nowRaw     = time(nullptr);
  rec.timestamp     = (nowRaw >= (time_t)MIN_VALID_EPOCH) ? (uint32_t)nowRaw : 0;
  rec.bootId        = currentBootId;
  rec.uptimeSec     = getUptimeSec();
  rec.windDir       = (uint16_t)((avgDir >= 0 && avgDir < 360) ? avgDir : 0);
  rec.windSpeed_x100 = (uint16_t)lroundf(constrain(wSpeed, 0.0f, 650.0f) * 100.0f);
  rec.windGust_x100  = (uint16_t)lroundf(constrain(maxGustKmh, 0.0f, 650.0f) * 100.0f);
  rec.rain_x100      = (uint32_t)lroundf(constrain(rainMM, 0.0f, 10000.0f) * 100.0f);

  if (bmeSampleCount > 0) {
    double n = (double)bmeSampleCount;
    rec.temp_x100 = (int16_t)llround((sumTemp / n) * 100.0);
    rec.hum_x100  = (uint16_t)llround(constrain(sumHum / n, 0.0, 100.0) * 100.0);
    rec.pres_x100 = (uint32_t)llround((sumPres / n) * 100.0);
    rec.gas_x100  = (uint32_t)llround(constrain(sumGas / n, 0.0, 100000.0) * 100.0);
    rec.flags     = REC_FLAG_BME_VALID;
  } else {
    rec.flags     = 0;
  }

  sumTemp = sumHum = sumPres = sumGas = 0.0;
  bmeSampleCount = 0;
  sumWindSin = sumWindCos = 0.0;
  windDirSamples = 0;
  maxGustKmh     = 0.0f;
}

void dispatchWindow(unsigned long currentMillis) {
  WeatherRecord rec;
  buildWindowRecord(rec, currentMillis);
  lastSendTime = currentMillis;

  if (!mqtt.submitRecord(rec)) {
    logMsg("[METEO] ERRORE: impossibile accodare il record!");
  }
}

void checkPublishWindow(unsigned long nowMs) {
  time_t nowRaw = time(nullptr);
  if (nowRaw >= (time_t)MIN_VALID_EPOCH) {
    uint32_t slot = (uint32_t)nowRaw / SEND_INTERVAL_SEC;
    if (lastAlignedSlot == 0 || slot + 1 < lastAlignedSlot) {
      lastAlignedSlot = slot;
      return;
    }
    if (slot > lastAlignedSlot) {
      lastAlignedSlot = slot;
      dispatchWindow(nowMs);
    }
  } else if (nowMs - lastSendTime >= SEND_INTERVAL_MS) {
    dispatchWindow(nowMs);
  }
}

// WEB DEBUG
void handleRoot() {
  char buf[220];
  time_t nowRaw = time(nullptr);
  char dateBuf[32] = "NTP non sincronizzato";
  if (nowRaw >= (time_t)MIN_VALID_EPOCH) {
    struct tm* ti = gmtime(&nowRaw);
    if (ti) strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d %H:%M:%S UTC", ti);
  }

  debugServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  debugServer.send(200, "text/html", "");

  debugServer.sendContent(
    "<html><head><meta charset='utf-8'><meta http-equiv='refresh' content='15'>"
    "<style>body{font-family:monospace;background:#111;color:#0f0;padding:10px}"
    "h2{color:#6cf}pre{white-space:pre-wrap;background:#000;padding:8px;border:1px solid #333}</style>"
    "</head><body><h2>Stazione Meteo ESP8266</h2>"
  );

  snprintf(buf, sizeof(buf),
           "<b>Uptime:</b> %lu s | <b>Free Heap:</b> %u B<br>"
           "<b>WiFi:</b> %s (%s, %d dBm)<br>"
           "<b>MQTT QoS1:</b> %s | <b>LittleFS:</b> %s<br>",
           (unsigned long)getUptimeSec(),
           (unsigned)ESP.getFreeHeap(),
           WiFi.status() == WL_CONNECTED ? "connesso" : "DISCONNESSO",
           WiFi.localIP().toString().c_str(),
           WiFi.RSSI(),
           mqtt.isConnected() ? "connesso" : "DISCONNESSO",
           queue.isReady() ? "OK" : "ERRORE");
  debugServer.sendContent(buf);

  snprintf(buf, sizeof(buf),
           "<b>BME680:</b> %s | <b>In coda:</b> %lu (seg: %lu, RAM: %u)<br>"
           "<b>Ora UTC:</b> %s (%lu)<hr><h2>Ultimi Log</h2><pre>",
           bmeFound ? "OK" : "NON trovato",
           (unsigned long)queue.pendingCount(),
           (unsigned long)queue.segmentCount(),
           (unsigned)queue.ramPendingCount(),
           dateBuf,
           (unsigned long)(nowRaw > 0 ? nowRaw : 0));
  debugServer.sendContent(buf);

  uint8_t start = (logCount == LOG_LINES) ? logHead : 0;
  for (uint8_t i = 0; i < logCount; i++) {
    uint8_t idx = (uint8_t)((start + i) % LOG_LINES);
    debugServer.sendContent(logRing[idx]);
    debugServer.sendContent("\n");
    yield();
  }

  debugServer.sendContent("</pre></body></html>");
  debugServer.sendContent("");
}

// WIFI + SNTP
void initWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long now = millis();
  lastWifiRetryTime = now;
  wifiLostAtMs      = now;
  wifiHardResetAtMs = now;
  logMsg("[WiFi] Avvio connessione a %s...", WIFI_SSID);
}

void maintainWiFi() {
  static bool wasConnected = false;
  bool isConnected = (WiFi.status() == WL_CONNECTED);

  if (isConnected && !wasConnected) {
    wasConnected = true;
    logMsg("[WiFi] Connesso: %s (%d dBm)", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    mqtt.notifyWifiReconnected();
  } else if (!isConnected) {
    unsigned long now = millis();
    if (wasConnected) {
      wasConnected      = false;
      lastWifiRetryTime = now;
      wifiLostAtMs      = now;
      wifiHardResetAtMs = now;
      logMsg("[WiFi] Connessione persa, salvo in Flash...");
    }

    if (now - wifiHardResetAtMs >= WIFI_HARD_RESET_MS) {
      wifiHardResetAtMs = now;
      lastWifiRetryTime = now;
      logMsg("[WiFi] Offline da 30m: reset interfaccia WiFi...");
      WiFi.disconnect(true);
      delay(100);
      WiFi.mode(WIFI_STA);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    } else if (now - lastWifiRetryTime >= WIFI_RETRY_MS) {
      lastWifiRetryTime = now;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }
}

void saveStateBeforeReboot(const char* reason, bool saveRtcClock) {
  logMsg("[SYS] Salvataggio stato (%s)...", reason);
  mqtt.cancelInFlightAndPreserve();

  unsigned long nowMs = millis();
  if (nowMs - lastSendTime >= 30000UL) {
    WeatherRecord rec;
    buildWindowRecord(rec, nowMs);
    StorageQueue::sealRecord(rec);
    queue.enqueue(rec);
  }

  queue.flushRamToFs();
  queue.flushMetaState();

  time_t nowRaw = time(nullptr);
  uint32_t nowTs = (nowRaw > 0) ? (uint32_t)nowRaw : 0;
  queue.saveLastKnownTime(nowTs, true);

  if (saveRtcClock && isTimeValid(nowTs)) {
    RtcClockStore st = { RTC_CLOCK_MAGIC, nowTs + 2UL, (RTC_CLOCK_MAGIC ^ (nowTs + 2UL)) };
    ESP.rtcUserMemoryWrite(0, (uint32_t*)&st, sizeof(st));
  }
}

void restoreClockFromRtcIfSoftRestart() {
  const rst_info* ri = ESP.getResetInfoPtr();
  RtcClockStore st = {};
  if (ri && ri->reason == REASON_SOFT_RESTART &&
      ESP.rtcUserMemoryRead(0, (uint32_t*)&st, sizeof(st)) &&
      st.magic == RTC_CLOCK_MAGIC &&
      st.crc == (st.magic ^ st.epoch) &&
      isTimeValid(st.epoch)) {
    struct timeval tv = {};
    tv.tv_sec = (time_t)st.epoch;
    settimeofday(&tv, nullptr);
    logMsg("[TIME] Orologio ripristinato da RTC RAM: %lu", (unsigned long)st.epoch);
  }
  RtcClockStore zero = {0, 0, 0};
  ESP.rtcUserMemoryWrite(0, (uint32_t*)&zero, sizeof(zero));
}

void checkSupervisor() {
  if (mqtt.consecutiveTimeouts() >= SUPERVISOR_MAX_MQTT_TIMEOUTS) {
    saveStateBeforeReboot("Troppi timeout PUBACK", true);
    mqtt.disconnect(false);
    delay(200);
    ESP.restart();
  }

  if (WiFi.status() != WL_CONNECTED &&
      (millis() - wifiLostAtMs >= SUPERVISOR_WIFI_DEAD_MS)) {
    saveStateBeforeReboot("WiFi offline > 2h", true);
    delay(200);
    ESP.restart();
  }
}

void setup() {
#if DEBUG
  Serial.begin(115200);
  delay(200);
#endif
  logMsg("[BOOT] Stazione Meteo ESP8266 QoS 1");

  restoreClockFromRtcIfSoftRestart();

  currentBootId = ESP.random();
  if (currentBootId == 0) currentBootId = 1;

  queue.begin();
  mqtt.begin(&queue, currentBootId);

  pinMode(RAIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(RAIN_PIN), handleRainInterrupt, FALLING);
  pinMode(WIND_SPEED_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(WIND_SPEED_PIN), handleWindSpeedInterrupt, FALLING);

  if (initBME680()) {
    logMsg("[BME680] Sensore inizializzato.");
    sampleBME680();
  } else {
    logMsg("[BME680] Sensore NON trovato (retry automatico).");
  }

  initWiFi();

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    otaUpdating = true;
    detachInterrupt(digitalPinToInterrupt(RAIN_PIN));
    detachInterrupt(digitalPinToInterrupt(WIND_SPEED_PIN));
    saveStateBeforeReboot("OTA", true);
    mqtt.disconnect(true);
  });
  ArduinoOTA.onError([](ota_error_t) {
    otaUpdating = false;
    attachInterrupt(digitalPinToInterrupt(RAIN_PIN), handleRainInterrupt, FALLING);
    attachInterrupt(digitalPinToInterrupt(WIND_SPEED_PIN), handleWindSpeedInterrupt, FALLING);
  });
  ArduinoOTA.begin();

  debugServer.on("/", handleRoot);
  debugServer.begin();

  noInterrupts();
  windClicks = 0;
  gustClicks = 0;
  rainClicks = 0;
  interrupts();

  unsigned long now = millis();
  lastSendTime      = now;
  lastGustCalcTime  = now;
  lastWindDirTime   = now;
  lastBmeSampleTime = now;
  lastBmeRetryTime  = now;
  lastReconcileTime = now;
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
  }
  if (otaUpdating) {
    yield();
    return;
  }

  debugServer.handleClient();
  maintainWiFi();

  unsigned long now = millis();

  if (now - lastGustCalcTime >= GUST_CALC_INTERVAL) {
    noInterrupts();
    unsigned long cGust = gustClicks;
    gustClicks = 0;
    interrupts();

    lastGustKmh = ((float)cGust / (GUST_CALC_INTERVAL / 1000.0f)) * WIND_SPEED_FACTOR;
    if (lastGustKmh > maxGustKmh) {
      maxGustKmh = lastGustKmh;
    }
    lastGustCalcTime = now;
  }

  if (now - lastWindDirTime >= WIND_DIR_SAMPLE_MS) {
    lastWindDirTime = now;
    sampleWindDirection();
  }

  if (bmeFound) {
    if (now - lastBmeSampleTime >= BME_SAMPLE_INTERVAL) {
      lastBmeSampleTime = now;
      sampleBME680();
    }
  } else if (now - lastBmeRetryTime >= BME_RETRY_INTERVAL) {
    lastBmeRetryTime = now;
    if (initBME680()) {
      logMsg("[BME680] Sensore ripristinato.");
      sampleBME680();
    }
  }

  checkPublishWindow(now);

  mqtt.process();

  if (now - lastReconcileTime >= QUEUE_RECONCILE_INTERVAL_MS) {
    lastReconcileTime = now;
    if (mqtt.isIdle() && queue.pendingCount() == 0) {
      queue.reconcileIfIdle();
    }
  }

  checkSupervisor();

  delay(5);
}
