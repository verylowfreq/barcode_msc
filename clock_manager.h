#pragma once

class ClockManager {
public:
  unsigned long epoch_millis = 0;
  unsigned long time_offset = 0;

  void begin() {
    epoch_millis = millis();
    time_offset = 0;
  }

  unsigned long get_current_time_millis() {
    unsigned long elapsed = millis() - epoch_millis;
    return elapsed + time_offset;
  }

  void set_clock(uint8_t hour, uint8_t minute, uint8_t second) {
    epoch_millis = millis();
    time_offset = ((unsigned long)hour * 3600 + (unsigned long)minute * 60 + (unsigned long)second) * 1000;
  }

  void get_clock(uint8_t* hour, uint8_t* minute, uint8_t* second) {
    unsigned long tm = get_current_time_millis() / 1000;
    uint8_t h = (tm / 3600) % 24;
    uint8_t m = (tm / 60) % 60;
    uint8_t s = (tm) % 60;
    if (hour) { *hour = h; }
    if (minute) { *minute = m; }
    if (second) { *second = s; }
  }

  uint8_t get_hour() {
    return (get_current_time_millis() / 1000 / 3600) % 24;
  }

  uint8_t get_minute() {
    return (get_current_time_millis() / 1000 / 60) % 60;
  }

  uint8_t get_second() {
    return (get_current_time_millis()/ 1000) % 60;
  }
};
