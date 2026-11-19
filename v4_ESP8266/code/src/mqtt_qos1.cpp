#include "mqtt_qos1.h"
#include <string.h>

int MqttQos1Client::encodeRemLen(uint8_t* dst, uint32_t len) {
  int idx = 0;
  do {
    uint8_t b = (uint8_t)(len % 128UL);
    len /= 128UL;
    if (len > 0) b |= 0x80;
    dst[idx++] = b;
  } while (len > 0 && idx < 4);
  return idx;
}

void MqttQos1Client::begin(StorageQueue* queue, uint32_t bootId) {
  _queue           = queue;
  _bootId          = bootId;
  _prevBootLastTs  = queue->lastKnownTime();
  _lastReconnectMs = millis() - MQTT_RECONNECT_BASE_MS;
}

void MqttQos1Client::resetRxParser() {
  _rxState      = RX_HEADER;
  _rxType       = 0;
  _rxRemLen     = 0;
  _rxMult       = 1;
  _rxPayloadIdx = 0;
}

void MqttQos1Client::scheduleDrainDelay(unsigned long delayMs) {
  _drainDelayStart = millis();
  _drainDelayMs    = delayMs;
  _drainWaiting    = true;
}

bool MqttQos1Client::isDrainReady() {
  if (!_drainWaiting) return true;
  if (millis() - _drainDelayStart >= _drainDelayMs) {
    _drainWaiting = false;
    return true;
  }
  return false;
}

void MqttQos1Client::cancelInFlightAndPreserve() {
  if (_inFlightKind == INFLIGHT_NONE) return;
  if (_inFlightKind == INFLIGHT_LIVE) {
    if (!_queue->enqueue(_inFlightRecord)) {
      logMsg("[MQTT] ERRORE: impossibile preservare record live in coda!");
    }
  }
  _inFlightKind    = INFLIGHT_NONE;
  _inFlightFromRam = false;
  _inFlightPktId   = 0;
  _inFlightEstTs   = 0;
}

void MqttQos1Client::disconnect(bool sendPacket) {
  bool wasReady = (_connState == CONN_READY);
  cancelInFlightAndPreserve();

  if (sendPacket && _net.connected()) {
    const uint8_t disc[2] = {0xE0, 0x00};
    _net.write(disc, 2);
    _net.flush();
    delay(20);
  }
  _net.stop();

  _connState    = CONN_OFF;
  _pingInFlight = false;
  resetRxParser();

  if (wasReady) {
    _queue->flushMetaState();
    logMsg("[MQTT] Disconnesso dal broker.");
  }
}

void MqttQos1Client::notifyWifiReconnected() {
  if (_connState != CONN_OFF) {
    disconnect(false);
  }
  _connectFailCnt  = 0;
  _lastReconnectMs = millis() - MQTT_RECONNECT_BASE_MS;
}

bool MqttQos1Client::sendConnectPacket() {
  const char* cid  = MQTT_CLIENT_NAME;
  const char* user = MQTT_USER;
  const char* pass = MQTT_PASS;

  uint16_t cidLen  = (uint16_t)strlen(cid);
  uint16_t userLen = (uint16_t)strlen(user);
  uint16_t passLen = (uint16_t)strlen(pass);
  bool hasUser     = (userLen > 0);
  bool hasPass     = (hasUser && passLen > 0);

  uint32_t remLen = 10UL + 2UL + cidLen
                  + (hasUser ? (2UL + userLen) : 0UL)
                  + (hasPass ? (2UL + passLen) : 0UL);
  if (remLen > 200UL) return false;

  uint8_t pkt[220];
  int p = 0;
  pkt[p++] = 0x10; // CONNECT
  p += encodeRemLen(&pkt[p], remLen);

  uint8_t flags = 0x02; // CleanSession
  if (hasUser) flags |= 0x80;
  if (hasPass) flags |= 0x40;

  pkt[p++] = 0x00; pkt[p++] = 0x04;
  pkt[p++] = 'M';  pkt[p++] = 'Q'; pkt[p++] = 'T'; pkt[p++] = 'T';
  pkt[p++] = 0x04; // MQTT 3.1.1
  pkt[p++] = flags;
  pkt[p++] = (uint8_t)(MQTT_KEEPALIVE_SEC >> 8);
  pkt[p++] = (uint8_t)(MQTT_KEEPALIVE_SEC & 0xFF);

  pkt[p++] = (uint8_t)(cidLen >> 8);
  pkt[p++] = (uint8_t)(cidLen & 0xFF);
  memcpy(&pkt[p], cid, cidLen);
  p += cidLen;

  if (hasUser) {
    pkt[p++] = (uint8_t)(userLen >> 8);
    pkt[p++] = (uint8_t)(userLen & 0xFF);
    memcpy(&pkt[p], user, userLen);
    p += userLen;
  }
  if (hasPass) {
    pkt[p++] = (uint8_t)(passLen >> 8);
    pkt[p++] = (uint8_t)(passLen & 0xFF);
    memcpy(&pkt[p], pass, passLen);
    p += passLen;
  }

  if (_net.write(pkt, (size_t)p) != (size_t)p) return false;
  _lastTxMs = millis();
  return true;
}

bool MqttQos1Client::sendPingReq() {
  const uint8_t ping[2] = {0xC0, 0x00};
  if (_net.write(ping, 2) != 2) return false;
  unsigned long now = millis();
  _lastTxMs     = now;
  _pingSentMs   = now;
  _pingInFlight = true;
  return true;
}

void MqttQos1Client::onPubAckReceived(uint16_t pktId) {
  if (_inFlightKind == INFLIGHT_NONE || pktId != _inFlightPktId) return;

  InFlight      completed    = _inFlightKind;
  bool          wasRam       = _inFlightFromRam;
  uint32_t      estTs        = _inFlightEstTs;
  WeatherRecord completedRec = _inFlightRecord;

  _inFlightKind        = INFLIGHT_NONE;
  _inFlightFromRam     = false;
  _inFlightPktId       = 0;
  _inFlightEstTs       = 0;
  _consecutiveTimeouts = 0;

  if (estTs > _lastEstSentTs) {
    _lastEstSentTs = estTs;
  }

  logMsg("[MQTT] PUBACK OK (id=%u)", (unsigned)pktId);

  if (completed == INFLIGHT_QUEUE) {
    _queue->advanceIfMatches(wasRam, completedRec);
    scheduleDrainDelay(DRAIN_PACE_MS);
  }
}

void MqttQos1Client::handleIncomingPacket() {
  uint8_t type = _rxType & 0xF0;

  if (type == 0x20 && _rxRemLen == 2) { // CONNACK
    if (_connState == CONN_WAIT_ACK) {
      if (_rxPayload[1] == 0x00) {
        _connState      = CONN_READY;
        _connectFailCnt = 0;
        _pingInFlight   = false;
        _lastRxMs       = millis();
        logMsg("[MQTT] Connesso (QoS 1 attivo).");
      } else {
        logMsg("[MQTT] CONNACK rifiutato: rc=%u", (unsigned)_rxPayload[1]);
        disconnect(false);
      }
    }
  } else if (type == 0x40 && _rxRemLen == 2) { // PUBACK
    uint16_t pid = ((uint16_t)_rxPayload[0] << 8) | _rxPayload[1];
    onPubAckReceived(pid);
  } else if (type == 0xD0 && _rxRemLen == 0) { // PINGRESP
    _pingInFlight = false;
  }
}

void MqttQos1Client::pollRx() {
  int budget = 64;
  while (_net.available() > 0 && budget-- > 0) {
    int c = _net.read();
    if (c < 0) break;
    uint8_t b = (uint8_t)c;
    _lastRxMs = millis();

    switch (_rxState) {
      case RX_HEADER:
        _rxType       = b;
        _rxRemLen     = 0;
        _rxMult       = 1;
        _rxPayloadIdx = 0;
        _rxState      = RX_REM_LEN;
        break;

      case RX_REM_LEN:
        _rxRemLen += (uint32_t)(b & 0x7F) * _rxMult;
        if ((b & 0x80) == 0) {
          if (_rxRemLen == 0) {
            handleIncomingPacket();
            _rxState = RX_HEADER;
          } else {
            _rxState = RX_PAYLOAD;
          }
        } else {
          _rxMult *= 128UL;
          if (_rxMult > (128UL * 128UL * 128UL)) {
            logMsg("[MQTT] Errore framing RX, riconnessione...");
            disconnect(false);
            return;
          }
        }
        break;

      case RX_PAYLOAD:
        if (_rxPayloadIdx < sizeof(_rxPayload)) {
          _rxPayload[_rxPayloadIdx] = b;
        }
        _rxPayloadIdx++;
        if (_rxPayloadIdx >= _rxRemLen) {
          handleIncomingPacket();
          _rxState = RX_HEADER;
        }
        break;
    }
  }
}

int MqttQos1Client::buildWeatherJson(const WeatherRecord& rec, char* buf, size_t bufSize, uint32_t& outEstTs) const {
  outEstTs     = 0;
  uint32_t ts  = rec.timestamp;
  time_t   nowRaw = time(nullptr);
  uint32_t now = (nowRaw > 0) ? (uint32_t)nowRaw : 0;
  bool ntpNow  = isTimeValid(now);

  if (!isTimeValid(ts)) {
    if (rec.bootId == _bootId && ntpNow) {
      uint32_t curUp = getUptimeSec();
      if (curUp >= rec.uptimeSec && now > (curUp - rec.uptimeSec)) {
        ts = now - (curUp - rec.uptimeSec);
      }
    } else if (rec.bootId != _bootId && isTimeValid(_prevBootLastTs)) {
      ts = _prevBootLastTs + rec.uptimeSec;
      if (ntpNow && ts >= now) {
        ts = now - 5UL;
      }
      if (_lastEstSentTs > 0 && ts <= _lastEstSentTs) {
        ts = _lastEstSentTs + 1UL;
      }
      outEstTs = ts;
    }
  }

  bool bmeValid = (rec.flags & REC_FLAG_BME_VALID) != 0;

  if (isTimeValid(ts)) {
    if (bmeValid) {
      return snprintf(
        buf, bufSize,
        "{\"timestamp\":%lu,\"wind_dir\":%u,\"wind_speed\":%.2f,\"wind_gust\":%.2f,"
        "\"rain_mm\":%.2f,\"temp\":%.2f,\"hum\":%.2f,\"pres\":%.2f,\"gas\":%.2f}",
        (unsigned long)ts,
        (unsigned)rec.windDir,
        rec.windSpeed_x100 / 100.0f,
        rec.windGust_x100 / 100.0f,
        rec.rain_x100 / 100.0f,
        rec.temp_x100 / 100.0f,
        rec.hum_x100 / 100.0f,
        rec.pres_x100 / 100.0f,
        rec.gas_x100 / 100.0f
      );
    }
    return snprintf(
      buf, bufSize,
      "{\"timestamp\":%lu,\"wind_dir\":%u,\"wind_speed\":%.2f,\"wind_gust\":%.2f,\"rain_mm\":%.2f}",
      (unsigned long)ts,
      (unsigned)rec.windDir,
      rec.windSpeed_x100 / 100.0f,
      rec.windGust_x100 / 100.0f,
      rec.rain_x100 / 100.0f
    );
  }

  if (bmeValid) {
    return snprintf(
      buf, bufSize,
      "{\"wind_dir\":%u,\"wind_speed\":%.2f,\"wind_gust\":%.2f,"
      "\"rain_mm\":%.2f,\"temp\":%.2f,\"hum\":%.2f,\"pres\":%.2f,\"gas\":%.2f}",
      (unsigned)rec.windDir,
      rec.windSpeed_x100 / 100.0f,
      rec.windGust_x100 / 100.0f,
      rec.rain_x100 / 100.0f,
      rec.temp_x100 / 100.0f,
      rec.hum_x100 / 100.0f,
      rec.pres_x100 / 100.0f,
      rec.gas_x100 / 100.0f
    );
  }
  return snprintf(
    buf, bufSize,
    "{\"wind_dir\":%u,\"wind_speed\":%.2f,\"wind_gust\":%.2f,\"rain_mm\":%.2f}",
    (unsigned)rec.windDir,
    rec.windSpeed_x100 / 100.0f,
    rec.windGust_x100 / 100.0f,
    rec.rain_x100 / 100.0f
  );
}

bool MqttQos1Client::publishWeatherQoS1(const WeatherRecord& rec, bool fromQueue, bool fromRam) {
  if (_connState != CONN_READY || !_net.connected()) return false;

  char payload[240];
  uint32_t estTs = 0;
  int payLen = buildWeatherJson(rec, payload, sizeof(payload), estTs);
  if (payLen <= 0 || payLen >= (int)sizeof(payload)) return false;

  uint16_t topLen = (uint16_t)strlen(MQTT_TOPIC);
  uint32_t remLen = 2UL + topLen + 2UL + (uint32_t)payLen;
  if (remLen + 5UL > 320UL) return false;

  uint16_t pktId = _nextPacketId++;
  if (_nextPacketId == 0) _nextPacketId = 1;

  uint8_t txBuf[320];
  int p = 0;
  txBuf[p++] = 0x32; // PUBLISH QoS 1
  p += encodeRemLen(&txBuf[p], remLen);
  txBuf[p++] = (uint8_t)(topLen >> 8);
  txBuf[p++] = (uint8_t)(topLen & 0xFF);
  memcpy(&txBuf[p], MQTT_TOPIC, topLen);
  p += topLen;
  txBuf[p++] = (uint8_t)(pktId >> 8);
  txBuf[p++] = (uint8_t)(pktId & 0xFF);
  memcpy(&txBuf[p], payload, (size_t)payLen);
  p += payLen;

  if (_net.write(txBuf, (size_t)p) != (size_t)p) {
    disconnect(false);
    return false;
  }

  unsigned long now = millis();
  _lastTxMs        = now;
  _inFlightKind    = fromQueue ? INFLIGHT_QUEUE : INFLIGHT_LIVE;
  _inFlightFromRam = fromRam;
  _inFlightPktId   = pktId;
  _inFlightStartMs = now;
  _inFlightEstTs   = estTs;
  _inFlightRecord  = rec;

  logMsg("[MQTT %s id=%u] %s", fromQueue ? "CODA" : "LIVE", (unsigned)pktId, payload);
  return true;
}

bool MqttQos1Client::submitRecord(const WeatherRecord& rec) {
  WeatherRecord sealed = rec;
  StorageQueue::sealRecord(sealed);

  if (_inFlightKind == INFLIGHT_LIVE) {
    if (!_queue->enqueue(_inFlightRecord)) return false;
    _inFlightKind  = INFLIGHT_NONE;
    _inFlightPktId = 0;
    _inFlightEstTs = 0;
  }

  if (_queue->pendingCount() == 0 && _connState == CONN_READY &&
      _inFlightKind == INFLIGHT_NONE && isTimeValid(sealed.timestamp)) {
    if (publishWeatherQoS1(sealed, false, false)) {
      return true;
    }
  }

  return _queue->enqueue(sealed);
}

void MqttQos1Client::process() {
  unsigned long nowMs = millis();

  time_t nowRaw = time(nullptr);
  uint32_t nowTs = (nowRaw > 0) ? (uint32_t)nowRaw : 0;
  bool ntpSynced = isTimeValid(nowTs);
  if (ntpSynced) {
    _queue->saveLastKnownTime(nowTs, false);
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (_connState != CONN_OFF) {
      disconnect(false);
    }
    return;
  }

  if (_connState == CONN_OFF) {
    unsigned long backoff = (_connectFailCnt >= 3) ? MQTT_RECONNECT_MAX_MS : MQTT_RECONNECT_BASE_MS;
    if (nowMs - _lastReconnectMs >= backoff) {
      _lastReconnectMs = nowMs;
      _net.stop();
      _net.setTimeout(MQTT_CONNECT_TIMEOUT_MS);

      if (_net.connect(MQTT_HOST, MQTT_PORT)) {
        _net.setNoDelay(true);
        resetRxParser();
        if (sendConnectPacket()) {
          _connState   = CONN_WAIT_ACK;
          _connStateMs = nowMs;
        } else {
          _net.stop();
          if (_connectFailCnt < 255) _connectFailCnt++;
        }
      } else {
        if (_connectFailCnt < 255) _connectFailCnt++;
        if (_connectFailCnt == 1 || (_connectFailCnt % 6) == 0) {
          logMsg("[MQTT] Broker irraggiungibile (%s:%u)", MQTT_HOST, (unsigned)MQTT_PORT);
        }
      }
    }
    return;
  }

  if (!_net.connected() && _net.available() == 0) {
    disconnect(false);
    scheduleDrainDelay(DRAIN_RETRY_MS);
    return;
  }

  pollRx();
  if (_connState == CONN_OFF) return;

  if (_connState == CONN_WAIT_ACK) {
    if (nowMs - _connStateMs >= MQTT_CONNACK_TIMEOUT_MS) {
      logMsg("[MQTT] Timeout CONNACK.");
      if (_connectFailCnt < 255) _connectFailCnt++;
      disconnect(false);
    }
    return;
  }

  if (_pingInFlight) {
    if (nowMs - _pingSentMs >= MQTT_PING_TIMEOUT_MS) {
      logMsg("[MQTT] Timeout PINGRESP, riconnessione...");
      disconnect(false);
      scheduleDrainDelay(DRAIN_RETRY_MS);
      return;
    }
  } else if ((nowMs - _lastTxMs >= MQTT_PING_INTERVAL_MS) ||
             (nowMs - _lastRxMs >= MQTT_PING_INTERVAL_MS)) {
    if (!sendPingReq()) {
      disconnect(false);
      scheduleDrainDelay(DRAIN_RETRY_MS);
      return;
    }
  }

  if (_inFlightKind != INFLIGHT_NONE) {
    if (nowMs - _inFlightStartMs >= MQTT_PUBACK_TIMEOUT_MS) {
      logMsg("[MQTT] Timeout PUBACK (id=%u), dato preservato.", (unsigned)_inFlightPktId);
      if (_consecutiveTimeouts < 255) _consecutiveTimeouts++;
      disconnect(false);
      scheduleDrainDelay(DRAIN_RETRY_MS);
    }
    return;
  }

  if (_queue->ramPendingCount() > 0) {
    _queue->flushRamToFs();
  }

  if (_queue->pendingCount() > 0 && isDrainReady()) {
    WeatherRecord rec;
    bool fromRam = false;

    if (!_queue->peek(rec, fromRam)) {
      _readErrorStreak++;
      logMsg("[FS] Errore lettura coda (%u/3).", _readErrorStreak);
      if (_readErrorStreak >= 3) {
        _readErrorStreak = 0;
        _queue->handleReadFailure();
      }
      scheduleDrainDelay(5000UL);
      return;
    }
    _readErrorStreak = 0;

    if (!StorageQueue::isRecordValid(rec)) {
      logMsg("[FS] Record corrotto (CRC/magic), saltato.");
      _queue->skipCurrent(fromRam);
      return;
    }

    if (!isTimeValid(rec.timestamp) && rec.bootId == _bootId && !ntpSynced) {
      scheduleDrainDelay(NTP_RETRY_WAIT_MS);
      return;
    }

    if (!isTimeValid(rec.timestamp) && rec.bootId != _bootId &&
        !isTimeValid(_prevBootLastTs) && !ntpSynced) {
      scheduleDrainDelay(NTP_RETRY_WAIT_MS);
      return;
    }

    if (!publishWeatherQoS1(rec, true, fromRam)) {
      scheduleDrainDelay(DRAIN_RETRY_MS);
    }
  }
}
