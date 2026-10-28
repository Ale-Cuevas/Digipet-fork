#pragma once

// SD card over SPI, backed by a host directory (sim::sdRoot()), so e.g.
// /digipet/genome.txt is a plain file you can read or drop in.

#include <Arduino.h>
#include <SPI.h>

#include <cstdio>
#include <string>
#include <sys/stat.h>

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

typedef enum { CARD_NONE, CARD_MMC, CARD_SD, CARD_SDHC, CARD_UNKNOWN } sdcard_type_t;

class File : public Print {
 public:
  File() = default;
  explicit File(FILE *handle) : handle_(handle) {}
  explicit operator bool() const { return handle_ != nullptr; }
  size_t write(uint8_t c) override { return handle_ ? fwrite(&c, 1, 1, handle_) : 0; }
  size_t write(const uint8_t *buffer, size_t size) override {
    return handle_ ? fwrite(buffer, 1, size, handle_) : 0;
  }
  using Print::write;
  size_t size() const {
    if (!handle_) return 0;
    struct stat info{};
    return fstat(fileno(handle_), &info) == 0 ? static_cast<size_t>(info.st_size) : 0;
  }
  int available() {
    if (!handle_) return 0;
    const long position = ftell(handle_);
    return position < 0 ? 0 : static_cast<int>(size() - static_cast<size_t>(position));
  }
  int read() {
    if (!handle_) return -1;
    const int c = fgetc(handle_);
    return c == EOF ? -1 : c;
  }
  String readStringUntil(char terminator) {
    std::string line;
    int c;
    while ((c = read()) >= 0 && c != terminator) line += static_cast<char>(c);
    return String(line);
  }
  void close() {
    if (handle_) fclose(handle_);
    handle_ = nullptr;
  }

 private:
  FILE *handle_ = nullptr;
};

class SDFS {
 public:
  bool begin(uint8_t, SPIClass &) {
    ::mkdir(sim::sdRoot().c_str(), 0755);
    return true;
  }
  void end() {}
  sdcard_type_t cardType() { return CARD_SDHC; }
  bool mkdir(const char *path) { return ::mkdir(hostPath(path).c_str(), 0755) == 0; }
  File open(const char *path, const char *mode = FILE_READ) {
    return File(fopen(hostPath(path).c_str(), mode));
  }

 private:
  static std::string hostPath(const char *path) { return sim::sdRoot() + path; }
};
inline SDFS SD;
