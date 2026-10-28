#pragma once

// Minimal stand-in for the Arduino core's String, wrapping std::string.
// Scoped to what this project's rendering code and Arduino_GFX/Print
// actually call -- not a general reimplementation of WString.h. The real
// ESP32 core's WString.cpp pulls in Arduino.h/esp32-hal-log.h and PSRAM-
// aware allocation tricks that have no native equivalent and aren't
// needed for rendering text into an in-memory canvas.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

class String {
 public:
  String() = default;
  String(const char *s) : value_(s ? s : "") {}
  String(const std::string &s) : value_(s) {}
  String(char c) : value_(1, c) {}
  String(int v) { value_ = std::to_string(v); }
  String(unsigned int v) { value_ = std::to_string(v); }
  String(long v) { value_ = std::to_string(v); }
  String(unsigned long v) { value_ = std::to_string(v); }
  String(long long v) { value_ = std::to_string(v); }
  String(unsigned long long v) { value_ = std::to_string(v); }
  String(float v, int decimals = 2) { value_ = formatFloat(v, decimals); }
  String(double v, int decimals = 2) { value_ = formatFloat(v, decimals); }

  const char *c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  bool isEmpty() const { return value_.empty(); }
  void reserve(size_t n) { value_.reserve(n); }
  void trim() {
    size_t start = value_.find_first_not_of(" \t\r\n");
    size_t end = value_.find_last_not_of(" \t\r\n");
    value_ = (start == std::string::npos) ? "" : value_.substr(start, end - start + 1);
  }

  bool startsWith(const String &prefix) const {
    return value_.compare(0, prefix.value_.size(), prefix.value_) == 0;
  }
  bool endsWith(const String &suffix) const {
    if (suffix.value_.size() > value_.size()) return false;
    return value_.compare(value_.size() - suffix.value_.size(), suffix.value_.size(),
                          suffix.value_) == 0;
  }
  int indexOf(char c) const {
    const size_t pos = value_.find(c);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  int indexOf(const String &s) const {
    const size_t pos = value_.find(s.value_);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  String substring(size_t from) const {
    return from >= value_.size() ? String("") : String(value_.substr(from));
  }
  String substring(size_t from, size_t to) const {
    if (from >= value_.size()) return String("");
    to = std::min(to, value_.size());
    if (to <= from) return String("");
    return String(value_.substr(from, to - from));
  }
  void replace(const String &from, const String &to) {
    if (from.value_.empty()) return;
    size_t pos = 0;
    while ((pos = value_.find(from.value_, pos)) != std::string::npos) {
      value_.replace(pos, from.value_.size(), to.value_);
      pos += to.value_.size();
    }
  }
  void remove(size_t index) {
    if (index < value_.size()) value_.erase(index);
  }
  void remove(size_t index, size_t count) {
    if (index < value_.size()) value_.erase(index, count);
  }
  void toLowerCase() {
    for (char &c : value_) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  }
  void toUpperCase() {
    for (char &c : value_) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  }
  void toCharArray(char *buf, size_t bufSize) const {
    if (!buf || bufSize == 0) return;
    const size_t n = std::min(bufSize - 1, value_.size());
    memcpy(buf, value_.data(), n);
    buf[n] = '\0';
  }
  long toInt() const { return strtol(value_.c_str(), nullptr, 10); }
  float toFloat() const { return strtof(value_.c_str(), nullptr); }

  char charAt(size_t i) const { return i < value_.size() ? value_[i] : '\0'; }
  char operator[](size_t i) const { return i < value_.size() ? value_[i] : '\0'; }

  String &operator=(const char *s) {
    value_ = s ? s : "";
    return *this;
  }
  String &operator+=(const String &other) {
    value_ += other.value_;
    return *this;
  }
  String &operator+=(const char *s) {
    value_ += s;
    return *this;
  }
  String &operator+=(char c) {
    value_ += c;
    return *this;
  }

  friend String operator+(String lhs, const String &rhs) {
    lhs.value_ += rhs.value_;
    return lhs;
  }
  friend String operator+(String lhs, const char *rhs) {
    lhs.value_ += rhs;
    return lhs;
  }
  friend String operator+(const char *lhs, const String &rhs) {
    return String(lhs) + rhs;
  }

  bool operator==(const String &other) const { return value_ == other.value_; }
  bool operator==(const char *other) const { return value_ == other; }
  bool operator!=(const String &other) const { return value_ != other.value_; }
  bool operator!=(const char *other) const { return value_ != other; }

 private:
  static std::string formatFloat(double v, int decimals) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
  }

  std::string value_;
};

// Real Arduino core: an opaque, never-instantiated type used only to tag a
// PROGMEM string pointer (via F()) as distinct from a plain const char* at
// the type level, so Print/Arduino_GFX can overload on it. Nothing on a
// native build actually lives in a separate PROGMEM address space, but the
// type still needs to stay distinct from char for those overloads to
// resolve the same way they do on the real target.
class __FlashStringHelper;
