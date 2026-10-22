#!/usr/bin/env python3
"""Compile the real logger against fake time/NVS and test retention and failures."""
import pathlib
import subprocess
import tempfile
root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as directory:
    work = pathlib.Path(directory)
    (work / 'Arduino.h').write_text(r'''
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
inline uint32_t nowMs = 0;
inline uint32_t millis() { return nowMs; }
struct SerialFake {
 std::string text;
 void println(const char *s) { text += s; text += "\n"; }
 template<class... T> void printf(const char *s, T... args) {
  char buffer[512]; snprintf(buffer, sizeof(buffer), s, args...); text += buffer;
 }
};
inline SerialFake Serial;
''')
    (work / 'esp_attr.h').write_text('#define RTC_DATA_ATTR\n')
    (work / 'Preferences.h').write_text(r'''
#pragma once
#include <map>
#include <string>
#include <cstring>
inline std::map<std::string,std::string> flash;
inline bool failWrite = false;
inline unsigned writes = 0;
struct Preferences {
 bool begin(const char *, bool) { return true; }
 bool isKey(const char *k) { return flash.count(k) != 0; }
 size_t getBytesLength(const char *k) { return flash[k].size(); }
 size_t getBytes(const char *k, void *p, size_t n) {
  if(flash[k].size() != n) return 0;
  memcpy(p,flash[k].data(),n); return n;
 }
 size_t putBytes(const char *k, const void *p, size_t n) {
  ++writes; if(failWrite) return 0;
  flash[k] = std::string(static_cast<const char *>(p),n); return n;
 }
};
''')
    (work / 'check.cpp').write_text(r'''
#include "power_telemetry.cpp"
#include <cassert>
int main() {
 PowerSnapshot battery{}; battery.flags = 8;
 powerTelemetryBegin(false);
 for(int i=0;i<3;++i) powerTelemetryRecord(PowerEvent::Sample,battery);
 assert(writes==0 && retained.pending.count==3);
 powerTelemetryBegin(true);
 assert(retained.pending.count==3 && retained.boot==2);
 PowerSnapshot usb=battery; usb.flags|=1;
 powerTelemetryRecord(PowerEvent::Sample,usb);
 assert(retained.pending.count==3);
 powerTelemetryRecord(PowerEvent::PowerConnected,usb);
 assert(writes==1 && retained.pending.count==0);
 powerTelemetryBegin(false);
 assert(retained.nextSequence==5 && retained.boot==3);
 powerTelemetryRecord(PowerEvent::Sample,battery);
 powerTelemetryDump();
 assert(Serial.text.find("POWERLOG BEGIN v1")!=std::string::npos);
 assert(Serial.text.find("5,3,")!=std::string::npos);
 assert(writes==1 && retained.pending.count==1); // export is read-only
 nowMs=300001;
 powerTelemetryRecord(PowerEvent::Sample,battery);
 assert(writes==2 && retained.pending.count==0);
 failWrite=true;
 for(int i=0;i<16;++i) powerTelemetryRecord(PowerEvent::Sample,battery);
 assert(retained.pending.count==16 && writes==3);
 for(int i=0;i<100;++i) powerTelemetryRecord(PowerEvent::State,battery);
 assert(writes==3 && retained.pending.count==16);
 failWrite=false; nowMs+=300001;
 powerTelemetryRecord(PowerEvent::Sample,battery);
 assert(writes==4 && retained.pending.count==1);
 for(int i=0;i<400;++i) powerTelemetryRecord(PowerEvent::Sample,battery);
 assert(retained.nextSlot<Slots);
 auto next=retained.nextSequence;
 powerTelemetryFlush();
 powerTelemetryBegin(false);
 assert(retained.nextSequence==next);
 unsigned total=0; Batch b{};
 for(unsigned i=0;i<Slots;++i) if(readBatch(i,b)) total+=b.count;
 assert(total<=256);
 puts("Power telemetry retention, export, wraparound and storage-failure checks passed");
}
''')
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-I'+str(work),
                    '-I'+str(root/'include'), '-I'+str(root/'src'),
                    str(work/'check.cpp'), '-o', str(work/'check')], check=True)
    subprocess.run([str(work/'check')], check=True)
