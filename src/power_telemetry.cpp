#include "power_telemetry.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <algorithm>
#include <cstring>

namespace {
constexpr uint32_t Magic = 0x50545731;
constexpr unsigned BatchSize = 16, Slots = 16;
struct Record {
  uint32_t sequence, boot, uptime;
  PowerSnapshot snapshot;
  uint8_t event;
  uint8_t reserved[3];
};
struct Batch {
  uint32_t magic;
  uint16_t count, reserved;
  Record records[BatchSize];
};
struct Retained {
  uint32_t magic, nextSequence, boot, nextSlot;
  Batch pending;
};
RTC_DATA_ATTR Retained retained{};
Preferences storage;
bool ready = false;
uint32_t lastFlush = 0;

void keyFor(unsigned slot, char *key) { snprintf(key, 8, "b%02u", slot); }
bool readBatch(unsigned slot, Batch &batch) {
  char key[8]; keyFor(slot, key);
  return storage.isKey(key) && storage.getBytesLength(key) == sizeof(batch) &&
      storage.getBytes(key, &batch, sizeof(batch)) == sizeof(batch) &&
      batch.magic == Magic && batch.count > 0 && batch.count <= BatchSize;
}
}

void powerTelemetryBegin(bool deepResume) {
  ready = storage.begin("powerlog", false);
  if (!ready) { Serial.println("Power telemetry: storage unavailable"); return; }
  if (!deepResume || retained.magic != Magic ||
      retained.pending.count > BatchSize || retained.nextSlot >= Slots) {
    retained = {};
    retained.magic = retained.pending.magic = Magic;
    retained.nextSequence = 1;
    Batch batch{};
    uint32_t newest = 0;
    for (unsigned slot = 0; slot < Slots; ++slot) {
      if (!readBatch(slot, batch)) continue;
      const Record &last = batch.records[batch.count - 1];
      if (last.sequence > newest) {
        newest = last.sequence;
        retained.nextSequence = newest + 1;
        retained.boot = last.boot;
        retained.nextSlot = (slot + 1) % Slots;
      }
    }
  }
  ++retained.boot;
  lastFlush = millis();
  Serial.println("Power telemetry: ready; send POWERLOG for CSV");
}

void powerTelemetryFlush() {
  if (!ready || !retained.pending.count) return;
  char key[8]; keyFor(retained.nextSlot, key);
  if (storage.putBytes(key, &retained.pending, sizeof(Batch)) != sizeof(Batch)) {
    Serial.println("Power telemetry: checkpoint failed; pending records retained");
    lastFlush = millis();
    return;
  }
  retained.nextSlot = (retained.nextSlot + 1) % Slots;
  retained.pending = {};
  retained.pending.magic = Magic;
  lastFlush = millis();
}

void powerTelemetryRecord(PowerEvent event, const PowerSnapshot &snapshot) {
  if (!ready) return;
  // Log on battery; retain the connection boundary so intervals are separable.
  if ((snapshot.flags & 1) && event != PowerEvent::PowerConnected) return;
  if (retained.pending.count == BatchSize) {
    // Retry at most every five minutes if storage is full/unavailable.
    if (millis() - lastFlush >= 300000) powerTelemetryFlush();
    if (retained.pending.count == BatchSize) return;
  }
  Record &record = retained.pending.records[retained.pending.count++];
  record = {};
  record.sequence = retained.nextSequence++;
  record.boot = retained.boot;
  record.uptime = millis();
  record.snapshot = snapshot;
  record.event = static_cast<uint8_t>(event);
  if (retained.pending.count == BatchSize || millis() - lastFlush >= 300000 ||
      event == PowerEvent::PowerConnected) powerTelemetryFlush();
}

void powerTelemetryDump() {
  if (!ready) { Serial.println("POWERLOG ERROR storage unavailable"); return; }
  // Read-only export includes uncheckpointed RTC records, without clearing them.
  static Record rows[Slots * BatchSize + BatchSize];
  unsigned count = 0;
  Batch batch{};
  for (unsigned slot = 0; slot < Slots; ++slot) {
    if (!readBatch(slot, batch)) continue;
    for (unsigned i = 0; i < batch.count; ++i) rows[count++] = batch.records[i];
  }
  for (unsigned i = 0; i < retained.pending.count; ++i)
    rows[count++] = retained.pending.records[i];
  std::sort(rows, rows + count, [](const Record &a, const Record &b) {
    return a.sequence < b.sequence;
  });
  Serial.println("POWERLOG BEGIN v1");
  Serial.println("sequence,boot,uptime_ms,epoch,event,millivolts,percent,brightness,page,wifi,battle,flags");
  uint32_t previous = 0;
  for (unsigned i = 0; i < count; ++i) {
    const Record &r = rows[i];
    if (r.sequence == previous) continue;
    previous = r.sequence;
    const PowerSnapshot &s = r.snapshot;
    Serial.printf("%lu,%lu,%lu,%lu,%u,%u,%u,%u,%u,%u,%u,%u\n",
      (unsigned long)r.sequence, (unsigned long)r.boot, (unsigned long)r.uptime,
      (unsigned long)s.epoch, r.event, s.millivolts, s.percent, s.brightness,
      s.page, s.wifi, s.battle, s.flags);
  }
  Serial.println("POWERLOG END");
}
