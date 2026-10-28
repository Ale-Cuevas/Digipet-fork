#pragma once

// NVS, persisted to a file by the sim host so the pet survives restarts
// exactly like it survives reboots on the board.

#include <Arduino.h>

class Preferences {
 public:
  bool begin(const char *name, bool readOnly = false, const char * = nullptr) {
    ns_ = name;
    readOnly_ = readOnly;
    return true;
  }
  void end() {}
  bool clear() {
    if (readOnly_) return false;
    sim::nvsClearNamespace(ns_);
    return true;
  }
  bool remove(const char *key) {
    if (readOnly_) return false;
    sim::nvsRemove(fullKey(key));
    return true;
  }
  bool isKey(const char *key) {
    std::string value;
    return sim::nvsGet(fullKey(key), value);
  }
  size_t getBytesLength(const char *key) {
    std::string value;
    return sim::nvsGet(fullKey(key), value) ? value.size() : 0;
  }
  size_t getBytes(const char *key, void *buf, size_t maxLength) {
    std::string value;
    if (!sim::nvsGet(fullKey(key), value) || value.size() > maxLength) return 0;
    memcpy(buf, value.data(), value.size());
    return value.size();
  }
  size_t putBytes(const char *key, const void *value, size_t length) {
    if (readOnly_) return 0;
    sim::nvsPut(fullKey(key), std::string(static_cast<const char *>(value), length));
    return length;
  }
  uint64_t getULong64(const char *key, uint64_t defaultValue = 0) {
    return getValue(key, defaultValue);
  }
  size_t putULong64(const char *key, uint64_t value) { return putBytes(key, &value, sizeof(value)); }
  uint32_t getUInt(const char *key, uint32_t defaultValue = 0) { return getValue(key, defaultValue); }
  size_t putUInt(const char *key, uint32_t value) { return putBytes(key, &value, sizeof(value)); }
  uint8_t getUChar(const char *key, uint8_t defaultValue = 0) { return getValue(key, defaultValue); }
  size_t putUChar(const char *key, uint8_t value) { return putBytes(key, &value, sizeof(value)); }
  bool getBool(const char *key, bool defaultValue = false) {
    return getValue<uint8_t>(key, defaultValue) != 0;
  }
  size_t putBool(const char *key, bool value) { return putUChar(key, value ? 1 : 0); }

 private:
  std::string fullKey(const char *key) const { return ns_ + "/" + key; }
  template <typename T>
  T getValue(const char *key, T defaultValue) {
    T value{};
    return getBytes(key, &value, sizeof(value)) == sizeof(value) ? value : defaultValue;
  }

  std::string ns_;
  bool readOnly_ = false;
};
