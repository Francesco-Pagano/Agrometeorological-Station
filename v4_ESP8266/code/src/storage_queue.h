#pragma once
#include <Arduino.h>
#include "config.h"

class StorageQueue {
public:
  bool     begin();
  bool     isReady() const { return _fsReady; }

  static void sealRecord(WeatherRecord& rec);
  static bool isRecordValid(const WeatherRecord& rec);
  static bool sameRecordIdentity(const WeatherRecord& a, const WeatherRecord& b);

  bool     enqueue(const WeatherRecord& rec);
  bool     peek(WeatherRecord& out, bool& fromRam);
  bool     advanceIfMatches(bool fromRam, const WeatherRecord& expected);
  void     skipCurrent(bool fromRam);
  void     handleReadFailure();
  void     flushRamToFs();
  void     flushMetaState();
  void     reconcileIfIdle();

  uint32_t pendingCount() const { return _fsQueuedRecords + (uint32_t)_ramCount; }
  uint32_t fsPendingCount() const { return _fsQueuedRecords; }
  size_t   ramPendingCount() const { return _ramCount; }
  uint32_t segmentCount() const { return _segmentCount; }

  uint32_t lastKnownTime() const { return _lastKnownTs; }
  void     saveLastKnownTime(uint32_t unixTs, bool force = false);

private:
  struct __attribute__((packed)) QueueMeta {
    uint32_t magic;
    uint32_t headSeg;
    uint32_t headOffset;
    uint32_t lastKnownTs;
    uint8_t  crc8;
  };

  void     loadMeta();
  bool     saveMetaAtomic();
  void     rebuildFromFs();
  bool     enqueueToFs(const WeatherRecord& rec);
  void     advanceInternal(bool fromRam);
  void     deleteHeadAndAdvance();
  void     dropOldestSegmentForSpace();
  void     resetEmptyFsState();
  uint32_t findLowestSegment() const;
  uint32_t getSegmentValidBytes(uint32_t segId) const;
  static void segmentPath(uint32_t segId, char* out, size_t outSize);
  static bool parseSegmentId(const char* name, uint32_t& outId);
  static uint8_t calcCrc8(const uint8_t* data, size_t len);

  bool          _fsReady         = false;
  uint32_t      _headSeg         = 0;
  uint32_t      _headOffset      = 0;
  uint32_t      _headValidBytes  = 0;
  uint32_t      _tailSeg         = 0;
  uint32_t      _tailRecords     = 0;
  bool          _tailMisaligned  = false;
  uint32_t      _segmentCount    = 0;
  uint32_t      _fsQueuedRecords = 0;
  uint32_t      _unsavedMetaAdv  = 0;

  uint32_t      _savedHeadSeg    = 0;
  uint32_t      _savedHeadOffset = 0;
  uint32_t      _lastKnownTs     = 0;

  WeatherRecord _ramBuf[RAM_FALLBACK_CAPACITY];
  size_t        _ramHead  = 0;
  size_t        _ramCount = 0;
};
