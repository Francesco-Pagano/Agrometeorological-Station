#include "offline_storage.h"
#include "config.h"
#include "debug_log.h"
#include "mqtt_manager.h"
#include <LittleFS.h>

static bool fsMounted = false;

bool storageBegin() {
  fsMounted = LittleFS.begin();
  if (!fsMounted) {
    logMsg("[FS] ERRORE: LittleFS.begin() fallito! Provo a formattare...");
    if (LittleFS.format()) {
      fsMounted = LittleFS.begin();
    }
  }
  logMsg(fsMounted ? "[FS] LittleFS montato correttamente." : "[FS] LittleFS NON disponibile.");
  return fsMounted;
}

bool storageIsMounted() {
  return fsMounted;
}

unsigned long countOfflineLines() {
  if (!fsMounted || !LittleFS.exists(OFFLINE_DATA_FILE)) return 0;
  File f = LittleFS.open(OFFLINE_DATA_FILE, "r");
  if (!f) return 0;
  unsigned long lines = 0;
  while (f.available()) {
    f.readStringUntil('\n');
    lines++;
  }
  f.close();
  return lines;
}

void saveOfflineData(const String &payload) {
  if (!fsMounted) {
    logMsg("[FS] LittleFS non montato: impossibile salvare il dato offline!");
    return;
  }

  // Safety cap: evita che il file cresca all'infinito se il server resta giu' a lungo
  if (countOfflineLines() >= OFFLINE_MAX_LINES) {
    logMsg("[FS] Limite righe offline raggiunto, dato scartato.");
    return;
  }

  File f = LittleFS.open(OFFLINE_DATA_FILE, "a");
  if (f) {
    f.println(payload);
    f.close();
    logMsg("[FS] Dato salvato offline: " + payload);
  } else {
    logMsg("[FS] ERRORE apertura file in append!");
  }
}

void sendOfflineData() {
  if (!fsMounted || !LittleFS.exists(OFFLINE_DATA_FILE)) return;

  File f = LittleFS.open(OFFLINE_DATA_FILE, "r");
  if (!f) return;

  String pending = "";
  unsigned long sentCount = 0;
  unsigned long failCount = 0;

  while (f.available()) {
    String payload = f.readStringUntil('\n');
    payload.trim();
    if (payload.length() == 0) continue;

    if (mqttClient.connected() && mqttClient.publish(mqtt_topic, payload.c_str())) {
      sentCount++;
      delay(50); // piccolo respiro per il broker/stack TCP
    } else {
      failCount++;
      pending += payload + "\n";
      // Se il client si e' disconnesso a meta' invio, tieni il resto per dopo
      if (!mqttClient.connected()) {
        while (f.available()) {
          pending += f.readStringUntil('\n') + "\n";
        }
        break;
      }
    }
  }
  f.close();

  // Riscrive il file solo con le righe non inviate (troncando quello vecchio)
  LittleFS.remove(OFFLINE_DATA_FILE);
  if (pending.length() > 0) {
    File wf = LittleFS.open(OFFLINE_DATA_FILE, "w");
    if (wf) {
      wf.print(pending);
      wf.close();
    }
  }

  if (sentCount > 0 || failCount > 0) {
    logMsg("[FS] Replay offline: inviati=" + String(sentCount) + " falliti=" + String(failCount));
  }
}
