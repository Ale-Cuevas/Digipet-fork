#pragma once

// RTC_DATA_ATTR variables survive deep sleep on the real chip. The sim
// gathers them in one linker section ("rtc_data", so GNU ld emits
// __start_/__stop_rtc_data) and carries that block across its own
// re-exec on wake -- see sim::deepSleep().
#define IRAM_ATTR
#define RTC_DATA_ATTR __attribute__((section("rtc_data")))
