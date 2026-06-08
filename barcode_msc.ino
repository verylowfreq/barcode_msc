#include <Adafruit_TinyUSB.h>
#include <KeyboardHost.h>
#include <MassStorageHost.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>


constexpr int PIN_LED_1 = PB15;
constexpr int PIN_LED_2 = PA8;
constexpr int PIN_LED_3 = PA9;
constexpr int PIN_LED_4 = PA10;

constexpr int PIN_BTN_1 = PB12;
constexpr int PIN_BTN_2 = PB13;
constexpr int PIN_BTN_3 = PB14;


#define SCREEN_ADDRESS 0x3C
// Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET)
Adafruit_SSD1306 display(128, 32, &Wire, -1);

Adafruit_USBH_Host USBHost;

typedef struct {
    bool is_hid_mounted;
    uint16_t hid_addr;
    uint16_t hid_instance;
    uint16_t hid_vid;
    uint16_t hid_pid;

    bool is_msc_mounted;
    uint16_t msc_addr;
    uint16_t msc_lun;
    uint16_t msc_vid;
    uint16_t msc_pid;

    bool is_passthrough_enabled;

    bool redraw_status;
    bool redraw_status_completed;
    unsigned long status_clear_timer;

    // This device is mounted as CDC device from PC.
    bool is_cdc_device_mounted;
} app_status_t;
static app_status_t app_status;

template<size_t BUFLEN>
class TextBuffer {
public:
    char buffer[BUFLEN];
    size_t end = 0;

    void begin() {
        clear();
    }

    size_t length() {
        return end;
    }

    bool is_full() {
        return length() == capacity();
    }

    bool is_empty() {
        return end == 0;
    }

    bool append_char(char ch) {
        if (length() + 1 > capacity()) {
            return false;
        } else {
            buffer[end] = ch;
            end += 1;
            buffer[end] = '\0';
            return true;
        }
    }

    bool append_str(const char* s) {
        size_t slen = strlen(s);
        if (length() + slen > capacity()) {
            return false;
        } else {
            memcpy(&buffer[end], s, slen);
            end += slen;
            buffer[end] = '\0';
            return true;
        }
    }

    bool truncate(size_t newsize) {
        if (newsize >= length()) {
            return false;
        } else {
            end = newsize;
            buffer[end] = '\0';
            return true;
        }
    }

    const char* get_str() {
        return buffer;
    }

    void clear() {
        end = 0;
        buffer[0] = '\0';
    }

    size_t capacity() const {
        return BUFLEN - 1;
    }

    void trim_end() {
        while (end > 0 &&
               (buffer[end - 1] == ' ' ||
                buffer[end - 1] == '\t' ||
                buffer[end - 1] == '\r' ||
                buffer[end - 1] == '\n')) {
          end -= 1;
        }
        buffer[end] = '\0';
    }

    bool equals(const char* s) {
        return strcmp(get_str(), s) == 0;
    }

    bool starts_with(const char* s) {
        return strncmp(get_str(), s, strlen(s)) == 0;
    }
};

static TextBuffer<256> textBuffer;
static TextBuffer<128> input_buffer;

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

ClockManager Clock;


void setup() {
  TinyUSBDevice.clearConfiguration();

  TinyUSBDevice.setID(0xf055, 0x6588);
  TinyUSBDevice.setProductDescriptor("HID Logger");
  TinyUSBDevice.setDeviceVersion(0x0001);

  pinMode(PIN_LED_1, OUTPUT);
  pinMode(PIN_LED_2, OUTPUT);
  pinMode(PIN_LED_3, OUTPUT);
  pinMode(PIN_LED_4, OUTPUT);

  pinMode(PIN_BTN_1, INPUT_PULLUP);
  pinMode(PIN_BTN_2, INPUT_PULLUP);
  pinMode(PIN_BTN_3, INPUT_PULLUP);

  // Wait OLED stable
  delay(50);
  display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  display.clearDisplay();
  display.display();

  draw_app_banner();

  // Enter to bootloader if BTN_3 (Right button) is pressed on boot
  if (digitalRead(PIN_BTN_3) == LOW) {
      reboot_bootloader();
  }

  Serial.setStringDescriptor("HID Logger");
  TinyUSBDevice.addInterface(Serial);
  Serial.begin(115200);

  Clock.begin();
  // Serial.printf("Clock: %02d:%02d:%02d\r\n", Clock.get_hour(), Clock.get_minute(), Clock.get_second());

  KeyboardHost.onMount([](uint8_t dev_addr, uint8_t instance, uint16_t vid, uint16_t pid) {
      if (!app_status.is_hid_mounted) {
          app_status.hid_addr = dev_addr;
          app_status.hid_instance = instance;
          app_status.hid_vid = vid;
          app_status.hid_pid = pid;
          app_status.is_hid_mounted = true;
          request_redraw_status();
      }
  });

  KeyboardHost.onUmount([](uint8_t dev_addr, uint8_t instance) {
      if (app_status.is_hid_mounted && app_status.hid_addr == dev_addr && app_status.hid_instance == instance) {
          app_status.is_hid_mounted = false;
          request_redraw_status();
      }
  });
  KeyboardHost.setLayout(KeyboardLayout::JP_JIS); // or KeyboardLayout::US_ASCII
  KeyboardHost.begin();

  MassStorageHost.onVolumeMount([](MassStorageVolumeInfo const& info) {
    // MassStorageVolumeInfo: dev_addr, lun, block_count, block_size
    if (!app_status.is_msc_mounted) {
        uint16_t vid, pid;
        if (!tuh_vid_pid_get(info.dev_addr, &vid, &pid)) {
          return;
        }
        app_status.msc_addr = info.dev_addr;
        app_status.msc_lun = info.lun;
        app_status.msc_vid = vid;
        app_status.msc_pid = pid;
        app_status.is_msc_mounted = true;
        request_redraw_status();
    }
  });
  MassStorageHost.onVolumeUmount([](uint8_t dev_addr, uint8_t lun) {
    if (app_status.is_msc_mounted && app_status.msc_addr == dev_addr && app_status.msc_lun == lun) {
        app_status.is_msc_mounted = false;
        request_redraw_status();
    }
  });

  MassStorageHost.onError([](uint8_t dev_addr, uint8_t lun, int32_t code) {
    (void)dev_addr; (void)lun; (void)code;
  });
  MassStorageHost.begin();

  USBHost.begin(0);

  // Request re-enumerate to the Host
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  display.setTextSize(1);
  display.setCursor(0, 24);
  display.print("Ready.");
  display.display();
  request_clear_after_millis(3000);
}

void loop() {
  USBHost.task();

  if (!app_status.is_cdc_device_mounted && Serial) {
      app_status.is_cdc_device_mounted = true;
      request_redraw_status();
  } else if (app_status.is_cdc_device_mounted && !Serial) {
      app_status.is_cdc_device_mounted = false;
      request_redraw_status();
  }

  unsigned long now = millis();
  if (!app_status.redraw_status_completed && (app_status.redraw_status || ((long)(now - app_status.status_clear_timer) > 0))) {
    display.fillRect(0, 24, 128, 8, SSD1306_BLACK);
    display.setTextSize(1);
    display.setCursor(0, 24);
    if (app_status.is_hid_mounted || app_status.is_msc_mounted || Serial) {
        display.print("Conn: ");
    }
    if (app_status.is_hid_mounted) {
        display.print("HID");
    }
    if (app_status.is_msc_mounted) {
        if (app_status.is_hid_mounted) {
            display.print("/");
        }
        display.print("MSC");
    }
    if (Serial) {
        if (app_status.is_hid_mounted || app_status.is_msc_mounted) {
            display.print("/");
        }
        display.print("cdc");
    }
    display.display();

    app_status.redraw_status = false;
    app_status.redraw_status_completed = true;
  }

  handle_button();

  handle_serial();

  if (KeyboardHost.available() > 0) {
    char ch = (char)KeyboardHost.read();
    if (ch != '\n' && !textBuffer.is_full()) {
        textBuffer.append_char(ch);

    } else { // ch == '\n' or buffer is full.
      display.setCursor(0, 24);
      display.setTextSize(1);
      display.fillRect(0, 24, 128, 8, SSD1306_BLACK);
      display.print(textBuffer.get_str());
      display.display();
      request_clear_after_millis(3000);

      if (!write_log(textBuffer.get_str()) && app_status.is_msc_mounted) {
        display.setCursor(0, 24);
        display.setTextSize(1);
        display.fillRect(0, 24, 128, 8, SSD1306_BLACK);
        display.print("MSC Error");
        display.display();
        request_clear_after_millis(3000);
      }

      if (app_status.is_passthrough_enabled) {
        write_with_clock(Serial, textBuffer.get_str());
        Serial.flush();
      }

      textBuffer.clear();
    }
  }
}

bool write_log(const char* msg) {
  if (!MassStorageHost.mounted()) { return false; }

  MscFile log_file = MassStorageHost.open("/log.txt", O_CREAT | O_RDWR | O_APPEND);
  if (!log_file) { return false; }

  write_with_clock(log_file, msg);
  log_file.flush();
  log_file.close();

  return true;
}

void write_with_clock(Stream& stream, const char* msg) {
  stream.printf("%02d:%02d:%02d\t", Clock.get_hour(), Clock.get_minute(), Clock.get_second());
  stream.print(msg);
  stream.print("\r\n");
}

void draw_app_banner() {
  display.fillRect(0, 0, 128, 16, SSD1306_BLACK);
  display.setCursor(0, 0);
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.println("HID Logger");
  display.display();
}


void reboot_bootloader() {
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.fillRect(0, 24, 128, 8, SSD1306_BLACK);
  display.setCursor(0, 24);
  display.print("(bootloader)");
  display.display();

  RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);
  PWR_BackupAccessCmd(ENABLE);
  BKP_WriteBackupRegister(BKP_DR10, 0x624c);
  NVIC_SystemReset();
}


void handle_button() {

  static bool is_press_started = false;
  static unsigned long press_start_millis = 0;
  if (digitalRead(PIN_BTN_1) == LOW) {
    if (!is_press_started) {
      press_start_millis = millis();
      is_press_started = true;
      display.clearDisplay();
      display.setCursor(0, 0);
      display.setTextSize(2);
      display.println("Set Clock");
      display.display();
    } else {
      const unsigned long press_duration = 2000;
      unsigned long elapsed = millis() - press_start_millis;
      uint8_t width = elapsed * 128 / press_duration;
      display.fillRect(0, 16, width, 4, SSD1306_WHITE);
      display.display();
      if (elapsed >= press_duration) {
        while (digitalRead(PIN_BTN_1) == LOW) { delay(10); }
        set_clock();
        is_press_started = false;
      }
    }
  } else if (is_press_started) {
    is_press_started = false;
    display.clearDisplay();
    draw_app_banner();
    request_redraw_status();
  }
}

void request_redraw_status() {
    app_status.redraw_status = true;
    app_status.redraw_status_completed = false;
}

void request_clear_after_millis(uint32_t millis_after) {
    app_status.status_clear_timer = millis() + millis_after;
    app_status.redraw_status_completed = false;
}

void set_clock() {
  display.clearDisplay();
  char digits[6] = {};

  {
    uint8_t h, m, s;
    Clock.get_clock(&h, &m, &s);
    digits[0] = h / 10;
    digits[1] = h % 10;
    digits[2] = m / 10;
    digits[3] = m % 10;
    digits[4] = s / 10;
    digits[5] = s % 10;
  }

  uint8_t cur = 0;
  while (true) {
    display.fillRect(0, 0, 128, 32, SSD1306_BLACK);
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.printf("%d%d:%d%d:%d%d", digits[0], digits[1], digits[2], digits[3], digits[4], digits[5]);
    uint8_t left = cur * 12 + (cur / 2) * 12;
    display.fillRect(left, 16, 10, 2, SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 24);
    display.print("[UP]   [NEXT]   [OK]");
    display.display();

    if (digitalRead(PIN_BTN_1) == LOW) {
      while (digitalRead(PIN_BTN_1) == LOW) { delay(10); }
      digits[cur] = (digits[cur] + 1) % 10;
      if (cur == 0 && digits[cur] == 3) { digits[cur] = 0; }
      if (digits[0] == 2 && digits[1] > 3) { digits[1] = 0; }
      if (cur == 2 && digits[cur] == 6) { digits[cur] = 0; }
      if (cur == 4 && digits[cur] == 6) { digits[cur] = 0; }
    }

    if (digitalRead(PIN_BTN_2) == LOW) {
      while (digitalRead(PIN_BTN_2) == LOW) { delay(10); }
      cur = (cur + 1) % 6;
    }

    if (digitalRead(PIN_BTN_3) == LOW) {
      while (digitalRead(PIN_BTN_3) == LOW) { delay(10); }
      break;
    }
  }

  {
    uint8_t h = digits[0] * 10 + digits[1];
    uint8_t m = digits[2] * 10 + digits[3];
    uint8_t s = digits[4] * 10 + digits[5];
    Clock.set_clock(h, m, s);
  }

  display.clearDisplay();

  draw_app_banner();
  display.setCursor(0, 24);
  display.setTextSize(1);
  display.printf("Clock: %02d:%02d:%02d\r\n", Clock.get_hour(), Clock.get_minute(), Clock.get_second());
  display.display();
  request_clear_after_millis(3000);
}

void handle_serial() {
  while (Serial.available() > 0) {
    char ch = (char)Serial.read();
    if (!input_buffer.append_char(ch)) {
        Serial.println("-ERR: input too long");
        input_buffer.clear();
        return;
    }

    if (ch != '\n') { continue; }
    input_buffer.trim_end();

    if (input_buffer.equals("status")) {
        Serial.println("+OK");
        if (!KeyboardHost.mounted()) {
            Serial.println("HID: unmounted");
        } else {
            Serial.printf("HID: mounted(0x%04x,0x%04x)", app_status.hid_vid, app_status.hid_pid);
            Serial.println();
        }
        if (!MassStorageHost.mounted()) {
            Serial.println("MSC: unmounted");
        } else {
            Serial.printf("MSC: mounted(0x%04x,0x%04x)", app_status.msc_vid, app_status.msc_pid);
            Serial.println();
        }
        {
            uint8_t h, m, s;
            Clock.get_clock(&h, &m, &s);
            Serial.printf("Clock: %02d:%02d:%02d", h, m, s);
            Serial.println();
        }

    } else if (input_buffer.equals("log get size")) {
        do {
        if (!MassStorageHost.mounted()) {
            Serial.println("-ERR: Not mounted.");
            break;
        }
        MscFile file = MassStorageHost.open("/log.txt", O_RDONLY);
        if (!file) {
            Serial.println("-ERR: log file not found.");
            break;
        }
        size_t filesize = file.size();
        file.close();
        Serial.printf("+OK: filesize=%u", filesize);
        Serial.println();
        } while (false);

    } else if (input_buffer.starts_with("log read ")) {
        do {
            uint32_t offset, length;
            sscanf(input_buffer.get_str(), "log read %u,%u", &offset, &length);
            MscFile file = MassStorageHost.open("/log.txt", O_RDONLY);
            if (!file) {
            Serial.println("-ERR: file not found");
            Serial.println();
            return;
            }
            size_t filesize = file.size();
            if (offset > filesize) {
                Serial.println("-ERR: Invalid offset");
                file.close();
                return;
            }
            if (offset + length > filesize) {
                Serial.println("-ERR: Invalid length");
                file.close();
                return;
            }
            file.seek(offset);
            Serial.printf("+OK: offset=%d,length=%d", offset, length);
            Serial.println();
            uint8_t buf[64];
            while (length > 0) {
                size_t chunksize = 63;
                if (chunksize > length) {
                chunksize = length;
                }
                size_t readlen = file.read(buf, chunksize);
                Serial.write(buf, readlen);
                Serial.flush();
                length -= readlen;
            }
            file.close();
            Serial.println();
        } while (false);

    } else if (input_buffer.equals("log clear")) {
        do {
            if (!MassStorageHost.mounted()) {
                Serial.println("-ERR: Not mounted.");
                break;
            }
            MscFile file = MassStorageHost.open("/log.txt", O_CREAT | O_WRITE | O_TRUNC);
            if (!file) {
            Serial.println("-ERR: log file not found.");
            break;
            }
            file.close();
            Serial.println("+OK: log file cleared.");
        } while (false);

    } else if (input_buffer.starts_with("clock set ")) {
        unsigned int h = 9, m = 0, s = 0;
        int num_of_fields = sscanf(input_buffer.get_str(), "clock set %u:%u:%u", &h, &m, &s);
        if (num_of_fields != 3) {
            Serial.println("-ERR: Invalid input.");
        } else {
            Clock.set_clock(h, m, s);
            Serial.printf("+OK: Clock is now %02d:%02d:%02d", h, m, s);
            Serial.println();
        }

    } else if (input_buffer.starts_with("dump set ")) {
        if (input_buffer.equals("dump set on")) {
            app_status.is_passthrough_enabled = true;
            Serial.println("+OK: passthrough enabled.");
        } else {
            app_status.is_passthrough_enabled = false;
            Serial.println("+OK: passthrough disabled.");
        }

    } else {
        Serial.println("-ERR: Unknown command.");
    }

    input_buffer.clear();
    return;
  }
}
