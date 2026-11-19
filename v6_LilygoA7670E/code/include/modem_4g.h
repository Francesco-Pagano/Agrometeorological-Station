#pragma once
#include <Arduino.h>
#include <time.h>
#include <esp_task_wdt.h>

#define TINY_GSM_MODEM_SIM7600
#define TINY_GSM_RX_BUFFER 1024
#include <TinyGsmClient.h>
#include "config.h"
#include "secrets.h"

TinyGsm modem(Serial1);
TinyGsmClient gsmClient(modem, 0);
bool modemPoweredUp = false;
unsigned long lastModemHardResetMillis = 0;
int8_t lastKnownCsq = -1;

void wakeModemDTR() {
  pinMode(MODEM_DTR_PIN, OUTPUT);
  digitalWrite(MODEM_DTR_PIN, LOW);
  delay(120);
}

void sleepModemDTR() {
  digitalWrite(MODEM_DTR_PIN, HIGH);
}

bool waitModemReadyAT(int maxTries = 25) {
  for (int i = 0; i < maxTries; i++) {
    esp_task_wdt_reset();
    if (modem.testAT(500)) {
      modem.sendAT("E0");
      modem.waitResponse(1000);
      modem.sendAT("+CTZU=0");
      modem.waitResponse(1000);
      modem.sendAT("+CSCLK=1");
      modem.waitResponse(1000);
      modemPoweredUp = true;
      sleepModemDTR();
      LOG_PRINTLN("[4G] Modem pronto.");
      return true;
    }
    delay(500);
  }
  modemPoweredUp = false;
  LOG_PRINTLN("[4G] ERRORE: Modem non risponde.");
  return false;
}

bool wakeModemAndVerifyAT() {
  wakeModemDTR();
  if (!modemPoweredUp) return false;

  Serial1.print("\r\n");
  delay(30);
  while (Serial1.available()) Serial1.read();

  for (int i = 0; i < 5; i++) {
    esp_task_wdt_reset();
    if (modem.testAT(400)) {
      return true;
    }
    delay(100);
  }
  LOG_PRINTLN("[4G] Modem non risponde dopo wake DTR: forzo riaccensione.");
  modemPoweredUp = false;
  return false;
}

bool powerOnModemHardware() {
  LOG_PRINTLN("[4G] Avvio hardware modem A7670E...");
  Serial1.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  wakeModemDTR();

  if (modem.testAT(400)) {
    LOG_PRINTLN("[4G] Modem già attivo: eseguo riavvio pulito via AT+CRESET...");
    modem.sendAT("+CRESET");
    modem.waitResponse(3000);
    modemPoweredUp = false;
    esp_task_wdt_reset();
    delay(5000);
    return waitModemReadyAT(25);
  }

  pinMode(MODEM_RESET_PIN, OUTPUT);
  digitalWrite(MODEM_RESET_PIN, HIGH);
  delay(500);
  digitalWrite(MODEM_RESET_PIN, LOW);
  delay(2000);

  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(100);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1000);
  digitalWrite(MODEM_PWRKEY, LOW);
  esp_task_wdt_reset();
  delay(3000);

  return waitModemReadyAT(25);
}

bool ensureCellularConnected() {
  if (!wakeModemAndVerifyAT()) {
    if (!powerOnModemHardware()) return false;
    wakeModemDTR();
  }

  while (Serial1.available()) Serial1.read();
  esp_task_wdt_reset();

  if (!modem.isNetworkConnected()) {
    if (!modem.waitForNetwork(20000L)) {
      LOG_PRINTLN("[4G] Rete Iliad assente.");
      sleepModemDTR();
      return false;
    }
  }

  esp_task_wdt_reset();
  if (!modem.isGprsConnected()) {
    if (!modem.gprsConnect(CELL_APN, CELL_USER, CELL_PASS)) {
      LOG_PRINTLN("[4G] Errore APN.");
      sleepModemDTR();
      return false;
    }
  }

  int16_t csq = modem.getSignalQuality();
  if (csq >= 0 && csq <= 31) {
    lastKnownCsq = (int8_t)csq;
  }

  delay(150);
  while (Serial1.available()) Serial1.read();
  return true;
}

void recoverModemConnection(uint8_t consecutiveFailures) {
  esp_task_wdt_reset();
  if (!modemPoweredUp) return;

  if (!wakeModemAndVerifyAT()) {
    if (lastModemHardResetMillis == 0 || (millis() - lastModemHardResetMillis >= MODEM_HARD_RESET_COOLDOWN_MS)) {
      lastModemHardResetMillis = millis();
      powerOnModemHardware();
    }
    return;
  }

  if (consecutiveFailures >= 6 &&
      (lastModemHardResetMillis == 0 || (millis() - lastModemHardResetMillis >= MODEM_HARD_RESET_COOLDOWN_MS))) {
    LOG_PRINTLN("[4G] Reboot pulito modem via AT+CRESET (cooldown 3h rispettato)...");
    lastModemHardResetMillis = millis();
    modem.gprsDisconnect();
    modem.sendAT("+CRESET");
    modem.waitResponse(3000);
    modemPoweredUp = false;
    esp_task_wdt_reset();
    delay(5000);
    waitModemReadyAT(25);
    return;
  } else if ((consecutiveFailures % 3) == 0) {
    LOG_PRINTLN("[4G] Riavvio stack radio (CFUN=0/1) per rinnovo PDP e flush DNS...");
    modem.gprsDisconnect();
    modem.sendAT("+CFUN=0");
    modem.waitResponse(5000);
    esp_task_wdt_reset();
    delay(1500);
    modem.sendAT("+CFUN=1");
    modem.waitResponse(5000);
    delay(500);
  }
  sleepModemDTR();
}

uint32_t getEpochFromModem() {
  if (!ensureCellularConnected()) return 0;

  int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
  float tz = 0;

  esp_task_wdt_reset();
  modem.NTPServerSync("pool.ntp.org", 0);
  delay(1500);

  bool ok = modem.getNetworkTime(&year, &month, &day, &hour, &min, &sec, &tz);
  sleepModemDTR();

  if (ok && year >= 2025) {
    struct tm t = {0};
    t.tm_year  = year - 1900;
    t.tm_mon   = month - 1;
    t.tm_mday  = day;
    t.tm_hour  = hour;
    t.tm_min   = min;
    t.tm_sec   = sec;
    t.tm_isdst = 0;

    uint32_t utcEpoch = (uint32_t)mktime(&t);
    if (utcEpoch >= MIN_VALID_EPOCH && utcEpoch <= MAX_VALID_EPOCH) {
      return utcEpoch;
    }
  }
  return 0;
}
