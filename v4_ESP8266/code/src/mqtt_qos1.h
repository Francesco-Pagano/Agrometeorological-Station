#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "config.h"
#include "storage_queue.h"

class MqttQos1Client {
public:
  void    begin(StorageQueue* queue, uint32_t bootId);
  void    disconnect(bool sendPacket = false);
  void    notifyWifiReconnected();

  bool    isConnected() const { return _connState == CONN_READY; }
  bool    isIdle() const { return _inFlightKind == INFLIGHT_NONE; }
  uint8_t consecutiveTimeouts() const { return _consecutiveTimeouts; }

  void    cancelInFlightAndPreserve();
  bool    submitRecord(const WeatherRecord& rec);
  void    process();

private:
  enum ConnState : uint8_t { CONN_OFF, CONN_WAIT_ACK, CONN_READY };
  enum RxState   : uint8_t { RX_HEADER, RX_REM_LEN, RX_PAYLOAD };
  enum InFlight  : uint8_t { INFLIGHT_NONE, INFLIGHT_LIVE, INFLIGHT_QUEUE };

  static int encodeRemLen(uint8_t* dst, uint32_t len);
  void       resetRxParser();
  void       pollRx();
  void       handleIncomingPacket();
  bool       sendConnectPacket();
  bool       sendPingReq();
  bool       publishWeatherQoS1(const WeatherRecord& rec, bool fromQueue, bool fromRam);
  int        buildWeatherJson(const WeatherRecord& rec, char* buf, size_t bufSize, uint32_t& outEstTs) const;

  void       scheduleDrainDelay(unsigned long delayMs);
  bool       isDrainReady();
  void       onPubAckReceived(uint16_t pktId);

  StorageQueue* _queue          = nullptr;
  WiFiClient    _net;
  uint32_t      _bootId         = 0;
  uint32_t      _prevBootLastTs = 0;
  uint32_t      _lastEstSentTs  = 0;

  ConnState     _connState      = CONN_OFF;
  unsigned long _connStateMs    = 0;
  unsigned long _lastReconnectMs = 0;
  uint8_t       _connectFailCnt = 0;

  unsigned long _lastTxMs       = 0;
  unsigned long _lastRxMs       = 0;
  bool          _pingInFlight   = false;
  unsigned long _pingSentMs     = 0;

  RxState       _rxState        = RX_HEADER;
  uint8_t       _rxType         = 0;
  uint32_t      _rxRemLen       = 0;
  uint32_t      _rxMult         = 1;
  uint32_t      _rxPayloadIdx   = 0;
  uint8_t       _rxPayload[4]   = {0};

  uint16_t      _nextPacketId   = 1;
  InFlight      _inFlightKind   = INFLIGHT_NONE;
  bool          _inFlightFromRam = false;
  uint16_t      _inFlightPktId  = 0;
  unsigned long _inFlightStartMs = 0;
  uint32_t      _inFlightEstTs  = 0;
  WeatherRecord _inFlightRecord = {};

  unsigned long _drainDelayStart = 0;
  unsigned long _drainDelayMs    = 0;
  bool          _drainWaiting    = false;
  uint8_t       _readErrorStreak = 0;
  uint8_t       _consecutiveTimeouts = 0;
};
