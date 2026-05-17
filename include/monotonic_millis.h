#pragma once

#include <Arduino.h>

class MonotonicMillis {
public:
  uint64_t now() {
    const uint32_t current = millis();
    if (current < lastLow_) {
      high_ += (1ULL << 32);
    }

    lastLow_ = current;
    return high_ | current;
  }

private:
  uint32_t lastLow_ = 0;
  uint64_t high_    = 0;
};