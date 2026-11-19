#include "storage_queue.h"
#include <LittleFS.h>
#include <string.h>

static const uint32_t META_MAGIC = 0x514D3031UL;
static const char*    META_FILE  = "/q_meta.bin";
static const char*    META_TMP   = "/q_meta.tmp";

uint8_t StorageQueue::calcCrc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

void StorageQueue::sealRecord(WeatherRecord& rec) {
  rec.magic = RECORD_MAGIC;
  rec.crc8  = calcCrc8((const uint8_t*)&rec, RECORD_SIZE - 1);
}

bool StorageQueue::isRecordValid(const WeatherRecord& rec) {
  if (rec.magic != RECORD_MAGIC) return false;
  return rec.crc8 == calcCrc8((const uint8_t*)&rec, RECORD_SIZE - 1);
}

bool StorageQueue::sameRecordIdentity(const WeatherRecord& a, const WeatherRecord& b) {
  return (a.timestamp == b.timestamp) &&
         (a.bootId    == b.bootId) &&
         (a.uptimeSec == b.uptimeSec) &&
         (a.crc8      == b.crc8);
}

void StorageQueue::segmentPath(uint32_t segId, char* out, size_t outSize) {
  snprintf(out, outSize, "/q_%08lu.bin", (unsigned long)segId);
}

bool StorageQueue::parseSegmentId(const char* name, uint32_t& outId) {
  if (!name) return false;
  const char* base = (name[0] == '/') ? (name + 1) : name;
  if (strlen(base) != 14) return false;
  if (strncmp(base, "q_", 2) != 0) return false;
  if (strcmp(base + 10, ".bin") != 0) return false;

  uint32_t val = 0;
  for (int i = 2; i < 10; i++) {
    if (base[i] < '0' || base[i] > '9') return false;
    val = val * 10UL + (uint32_t)(base[i] - '0');
  }
  if (val == 0) return false;
  outId = val;
  return true;
}

uint32_t StorageQueue::getSegmentValidBytes(uint32_t segId) const {
  char path[24];
  segmentPath(segId, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f) return 0;
  uint32_t sz = (uint32_t)f.size();
  f.close();
  return (sz / RECORD_SIZE) * RECORD_SIZE;
}

uint32_t StorageQueue::findLowestSegment() const {
  uint32_t minSeg = 0;
  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    yield();
    uint32_t id = 0;
    if (parseSegmentId(dir.fileName().c_str(), id)) {
      if (minSeg == 0 || id < minSeg) minSeg = id;
    }
  }
  return minSeg;
}

void StorageQueue::loadMeta() {
  _savedHeadSeg    = 0;
  _savedHeadOffset = 0;
  _lastKnownTs     = 0;

  const char* src = LittleFS.exists(META_FILE) ? META_FILE
                  : (LittleFS.exists(META_TMP) ? META_TMP : nullptr);
  if (!src) return;

  File f = LittleFS.open(src, "r");
  if (!f) return;

  QueueMeta m = {};
  size_t n = f.read((uint8_t*)&m, sizeof(QueueMeta));
  f.close();

  if (n == sizeof(QueueMeta) &&
      m.magic == META_MAGIC &&
      m.crc8 == calcCrc8((const uint8_t*)&m, sizeof(QueueMeta) - 1)) {
    _savedHeadSeg    = m.headSeg;
    _savedHeadOffset = m.headOffset;
    _lastKnownTs     = m.lastKnownTs;
  }
}

bool StorageQueue::saveMetaAtomic() {
  if (!_fsReady) return false;

  QueueMeta m = {};
  m.magic       = META_MAGIC;
  m.headSeg     = _headSeg;
  m.headOffset  = _headOffset;
  m.lastKnownTs = _lastKnownTs;
  m.crc8        = calcCrc8((const uint8_t*)&m, sizeof(QueueMeta) - 1);

  File f = LittleFS.open(META_TMP, "w");
  if (!f) return false;
  size_t w = f.write((const uint8_t*)&m, sizeof(QueueMeta));
  f.flush();
  f.close();
  if (w != sizeof(QueueMeta)) {
    LittleFS.remove(META_TMP);
    return false;
  }

  if (!LittleFS.rename(META_TMP, META_FILE)) {
    LittleFS.remove(META_FILE);
    if (!LittleFS.rename(META_TMP, META_FILE)) {
      return false;
    }
  }

  _savedHeadSeg    = _headSeg;
  _savedHeadOffset = _headOffset;
  _unsavedMetaAdv  = 0;
  return true;
}

void StorageQueue::flushMetaState() {
  if (_unsavedMetaAdv > 0) {
    saveMetaAtomic();
  }
}

void StorageQueue::saveLastKnownTime(uint32_t unixTs, bool force) {
  if (!isTimeValid(unixTs)) return;
  if (force || _lastKnownTs == 0 ||
      (unixTs > _lastKnownTs && (unixTs - _lastKnownTs) >= LAST_TS_SAVE_INTERVAL_SEC)) {
    _lastKnownTs = unixTs;
    saveMetaAtomic();
  }
}

void StorageQueue::resetEmptyFsState() {
  _headSeg         = 0;
  _headOffset      = 0;
  _headValidBytes  = 0;
  _tailSeg         = 0;
  _tailRecords     = 0;
  _tailMisaligned  = false;
  _segmentCount    = 0;
  _fsQueuedRecords = 0;
  saveMetaAtomic();
}

void StorageQueue::rebuildFromFs() {
  if (!_fsReady) return;

  uint32_t badSegs[24];
  size_t   badCount     = 0;
  uint32_t minSeg       = 0;
  uint32_t maxSeg       = 0;
  uint32_t headSize     = 0;
  uint32_t tailSize     = 0;
  uint32_t countSegs    = 0;
  uint32_t totalRawRecs = 0;

  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    yield();
    uint32_t id = 0;
    if (parseSegmentId(dir.fileName().c_str(), id)) {
      uint32_t sz = (uint32_t)dir.fileSize();
      if (sz < RECORD_SIZE) {
        if (badCount < 24) badSegs[badCount++] = id;
      } else {
        countSegs++;
        totalRawRecs += (sz / RECORD_SIZE);
        if (minSeg == 0 || id < minSeg) {
          minSeg   = id;
          headSize = sz;
        }
        if (id > maxSeg) {
          maxSeg   = id;
          tailSize = sz;
        }
      }
    }
  }

  for (size_t i = 0; i < badCount; i++) {
    char p[24];
    segmentPath(badSegs[i], p, sizeof(p));
    LittleFS.remove(p);
  }

  if (countSegs == 0) {
    resetEmptyFsState();
    return;
  }

  _headSeg        = minSeg;
  _tailSeg        = maxSeg;
  _segmentCount   = countSegs;
  _headValidBytes = (headSize / RECORD_SIZE) * RECORD_SIZE;
  _tailRecords    = tailSize / RECORD_SIZE;
  _tailMisaligned = ((tailSize % RECORD_SIZE) != 0);

  if (_savedHeadSeg == _headSeg) {
    _headOffset = (_savedHeadOffset / RECORD_SIZE) * RECORD_SIZE;
  } else {
    _headOffset = 0;
    saveMetaAtomic();
  }

  uint32_t alreadyRead = (_headOffset <= _headValidBytes)
                         ? (_headOffset / RECORD_SIZE)
                         : (_headValidBytes / RECORD_SIZE);
  _fsQueuedRecords = (totalRawRecs > alreadyRead) ? (totalRawRecs - alreadyRead) : 0;

  while (_headSeg != 0 && _headOffset >= _headValidBytes) {
    deleteHeadAndAdvance();
  }
}

void StorageQueue::reconcileIfIdle() {
  if (!_fsReady || _ramCount > 0 || _fsQueuedRecords > 0) return;
  if (findLowestSegment() != 0) {
    logMsg("[FS] Riconciliazione: trovati segmenti residui, ricostruisco stato.");
    rebuildFromFs();
  }
}

bool StorageQueue::begin() {
  _fsReady = LittleFS.begin();
  if (!_fsReady) {
    logMsg("[FS] LittleFS.begin() fallito, formatto...");
    if (LittleFS.format()) {
      _fsReady = LittleFS.begin();
    }
  }
  if (!_fsReady) {
    logMsg("[FS] ERRORE: LittleFS non disponibile, attivo solo buffer RAM.");
    return false;
  }

  if (LittleFS.exists("/offline_data.jsonl")) LittleFS.remove("/offline_data.jsonl");
  if (LittleFS.exists("/offline_temp.jsonl")) LittleFS.remove("/offline_temp.jsonl");

  loadMeta();
  rebuildFromFs();

  logMsg("[FS] Coda pronta | Seg: %lu | Record Flash: %lu",
         (unsigned long)_segmentCount, (unsigned long)_fsQueuedRecords);
  return true;
}

void StorageQueue::deleteHeadAndAdvance() {
  if (_headSeg == 0) return;

  uint32_t oldHead = _headSeg;
  char path[24];
  segmentPath(oldHead, path, sizeof(path));
  LittleFS.remove(path);

  if (_segmentCount > 0) _segmentCount--;

  uint32_t nextSeg = findLowestSegment();
  if (nextSeg == 0) {
    resetEmptyFsState();
    logMsg("[FS] Coda Flash svuotata.");
  } else {
    _headSeg        = nextSeg;
    _headOffset     = 0;
    _headValidBytes = getSegmentValidBytes(_headSeg);
    saveMetaAtomic();
    logMsg("[FS] Seg %lu rimosso, passo al %lu (residui: %lu)",
           (unsigned long)oldHead, (unsigned long)_headSeg, (unsigned long)_fsQueuedRecords);
  }
}

void StorageQueue::dropOldestSegmentForSpace() {
  if (_headSeg == 0) return;

  uint32_t droppedRecs = (_headValidBytes > _headOffset)
                         ? ((_headValidBytes - _headOffset) / RECORD_SIZE) : 0;
  _fsQueuedRecords = (_fsQueuedRecords > droppedRecs) ? (_fsQueuedRecords - droppedRecs) : 0;

  logMsg("[FS] Spazio limite: rimosso seg %lu (%lu rec)",
         (unsigned long)_headSeg, (unsigned long)droppedRecs);
  deleteHeadAndAdvance();
}

bool StorageQueue::enqueueToFs(const WeatherRecord& rec) {
  if (!_fsReady) return false;

  bool needNewSegment = (_tailSeg == 0) || (_tailRecords >= SEGMENT_MAX_RECORDS) || _tailMisaligned;

  if (needNewSegment) {
    while (_segmentCount >= MAX_SEGMENTS && _headSeg != 0) {
      dropOldestSegmentForSpace();
    }
    while (_headSeg != 0) {
      FSInfo info;
      if (!LittleFS.info(info)) break;
      size_t freeB = (info.totalBytes > info.usedBytes) ? (info.totalBytes - info.usedBytes) : 0;
      if (freeB >= MIN_FREE_FS_BYTES) break;
      dropOldestSegmentForSpace();
    }

    uint32_t newSeg = (_tailSeg == 0) ? 1UL : (_tailSeg + 1UL);
    char path[24];
    segmentPath(newSeg, path, sizeof(path));

    File f = LittleFS.open(path, "w");
    if (!f) return false;

    size_t written = f.write((const uint8_t*)&rec, RECORD_SIZE);
    f.flush();
    f.close();

    if (written != RECORD_SIZE) {
      LittleFS.remove(path);
      return false;
    }

    _tailSeg        = newSeg;
    _tailRecords    = 1;
    _tailMisaligned = false;
    _segmentCount++;
    _fsQueuedRecords++;

    if (_headSeg == 0) {
      _headSeg        = newSeg;
      _headOffset     = 0;
      _headValidBytes = RECORD_SIZE;
      saveMetaAtomic();
    }

    logMsg("[FS] Nuovo seg %lu (totale in coda: %lu)",
           (unsigned long)newSeg, (unsigned long)pendingCount());
    return true;
  }

  char path[24];
  segmentPath(_tailSeg, path, sizeof(path));
  File f = LittleFS.open(path, "a");
  if (!f) {
    _tailMisaligned = true;
    return false;
  }

  size_t written = f.write((const uint8_t*)&rec, RECORD_SIZE);
  f.flush();
  f.close();

  if (written != RECORD_SIZE) {
    _tailMisaligned = true;
    return false;
  }

  _tailRecords++;
  _fsQueuedRecords++;
  if (_headSeg == _tailSeg) {
    _headValidBytes += RECORD_SIZE;
  }

  logMsg("[FS] Salvato in seg %lu (in coda: %lu)",
         (unsigned long)_tailSeg, (unsigned long)pendingCount());
  return true;
}

void StorageQueue::flushRamToFs() {
  if (!_fsReady || _ramCount == 0) return;

  while (_ramCount > 0) {
    if (!enqueueToFs(_ramBuf[_ramHead])) break;
    _ramHead = (_ramHead + 1) % RAM_FALLBACK_CAPACITY;
    _ramCount--;
  }
}

bool StorageQueue::enqueue(const WeatherRecord& rec) {
  if (_ramCount == 0 && enqueueToFs(rec)) {
    return true;
  }

  if (_ramCount < RAM_FALLBACK_CAPACITY) {
    size_t idx = (_ramHead + _ramCount) % RAM_FALLBACK_CAPACITY;
    _ramBuf[idx] = rec;
    _ramCount++;
    logMsg("[FS] Accodato in RAM fallback (%u/%u)",
           (unsigned)_ramCount, (unsigned)RAM_FALLBACK_CAPACITY);
    return true;
  }

  flushRamToFs();
  if (_ramCount < RAM_FALLBACK_CAPACITY) {
    size_t idx = (_ramHead + _ramCount) % RAM_FALLBACK_CAPACITY;
    _ramBuf[idx] = rec;
    _ramCount++;
    return true;
  }

  logMsg("[FS] ERRORE: sia LittleFS che buffer RAM pieni!");
  return false;
}

bool StorageQueue::peek(WeatherRecord& out, bool& fromRam) {
  if (_fsQueuedRecords > 0 && _headSeg == 0) {
    rebuildFromFs();
  }

  if (_fsReady && _headSeg != 0) {
    while (_headSeg != 0 && _headOffset >= _headValidBytes) {
      deleteHeadAndAdvance();
    }
    if (_headSeg != 0) {
      char path[24];
      segmentPath(_headSeg, path, sizeof(path));
      File f = LittleFS.open(path, "r");
      if (!f) return false;

      if (!f.seek(_headOffset, SeekSet)) {
        f.close();
        return false;
      }

      size_t nRead = f.read((uint8_t*)&out, RECORD_SIZE);
      f.close();
      if (nRead == RECORD_SIZE) {
        fromRam = false;
        return true;
      }
      return false;
    }
  }

  if (_ramCount > 0) {
    out     = _ramBuf[_ramHead];
    fromRam = true;
    return true;
  }

  return false;
}

void StorageQueue::advanceInternal(bool fromRam) {
  if (fromRam) {
    if (_ramCount > 0) {
      _ramHead = (_ramHead + 1) % RAM_FALLBACK_CAPACITY;
      _ramCount--;
    }
    return;
  }

  if (_headSeg == 0) return;

  _headOffset += RECORD_SIZE;
  if (_fsQueuedRecords > 0) _fsQueuedRecords--;
  _unsavedMetaAdv++;

  if (_headOffset >= _headValidBytes) {
    deleteHeadAndAdvance();
  } else if (_unsavedMetaAdv >= META_SAVE_EVERY_RECS) {
    saveMetaAtomic();
  }
}

bool StorageQueue::advanceIfMatches(bool fromRam, const WeatherRecord& expected) {
  WeatherRecord currentHead;
  bool headIsRam = false;
  if (!peek(currentHead, headIsRam)) return false;

  if (headIsRam != fromRam || !sameRecordIdentity(currentHead, expected)) {
    logMsg("[FS] Testa coda cambiata durante in-flight: advance saltato.");
    return false;
  }
  advanceInternal(fromRam);
  return true;
}

void StorageQueue::skipCurrent(bool fromRam) {
  advanceInternal(fromRam);
}

void StorageQueue::handleReadFailure() {
  if (_headSeg == 0) {
    rebuildFromFs();
    return;
  }

  char path[24];
  segmentPath(_headSeg, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (f) {
    uint32_t validBytes = ((uint32_t)f.size() / RECORD_SIZE) * RECORD_SIZE;
    f.close();
    _headValidBytes = validBytes;

    if (_headOffset + RECORD_SIZE <= validBytes) {
      logMsg("[FS] Salto singolo record illeggibile in seg %lu", (unsigned long)_headSeg);
      advanceInternal(false);
      flushMetaState();
      return;
    }
    deleteHeadAndAdvance();
    return;
  }

  logMsg("[FS] Seg %lu non apribile: rimozione e ricostruzione.", (unsigned long)_headSeg);
  LittleFS.remove(path);
  _savedHeadSeg    = 0;
  _savedHeadOffset = 0;
  rebuildFromFs();
}
