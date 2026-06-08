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

    // This device is mounted as CDC device from PC.
    bool is_cdc_device_mounted;
} app_status_t;
static app_status_t app_status;


template<typename T, size_t BUFLEN>
class RingBuffer {
public:
  size_t next_write_idx = 0;
  size_t next_read_idx = 0;
  T buffer[BUFLEN];

  void begin() {
    next_write_idx = 0;
    next_read_idx = 0;
  }

  bool is_empty() {
    return next_write_idx == next_read_idx;
  }

  bool is_full() {
    return (next_write_idx + 1) % BUFLEN == next_read_idx;
  }

  bool push(T val) {
    if (is_full()) { return false; }
    buffer[next_write_idx] = val;
    next_write_idx = (next_write_idx + 1) % BUFLEN;
    return true;
  }

  bool pop(T* val) {
    if (is_empty()) {
      return false;
    }
    if (val != nullptr) {
      *val = buffer[next_read_idx];
    }
    next_read_idx = (next_read_idx + 1) % BUFLEN;
    return true;
  }
};

static RingBuffer<char, 128> input_buffer;

constexpr size_t buflen = 128;
static uint8_t buf[128];

unsigned long status_clear_timer = 0;


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
          app_status.redraw_status = true;
      }
  });

  KeyboardHost.onUmount([](uint8_t dev_addr, uint8_t instance) {
      if (app_status.is_hid_mounted && app_status.hid_addr == dev_addr && app_status.hid_instance == instance) {
          app_status.is_hid_mounted = false;
          app_status.redraw_status = true;
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
        app_status.redraw_status = true;
    }
  });
  MassStorageHost.onVolumeUmount([](uint8_t dev_addr, uint8_t lun) {
    if (app_status.is_msc_mounted && app_status.msc_addr == dev_addr && app_status.msc_lun == lun) {
        app_status.is_msc_mounted = false;
        app_status.redraw_status = true;
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
  status_clear_timer = millis() + 3000;
}

void loop() {
  USBHost.task();

  if ((!app_status.is_cdc_device_mounted && Serial) || (app_status.is_cdc_device_mounted && !Serial)) {
    app_status.is_cdc_device_mounted = true;
  }

  unsigned long now = millis();
  if (app_status.redraw_status || ((long)(now - status_clear_timer) > 0)) {
    display.fillRect(0, 24, 128, 8, SSD1306_BLACK);
    display.setTextSize(1);
    if (app_status.is_hid_mounted) {
        display.setCursor(0, 24);
        display.print("[HID]");
    }
    if (app_status.is_msc_mounted) {
        display.setCursor(5 * 7, 24);
        display.print("[MSC]");
    }
    if (Serial) {
        display.setCursor(5 * 7 + 5 * 7, 24);
        display.print("[cdc]");
    }
    display.display();

    app_status.redraw_status = false;
  }

  handle_button();

  handle_serial();

  if (KeyboardHost.available() > 0) {
    char ch = (char)KeyboardHost.read();
    input_buffer.push(ch);

    if (ch == '\n') {
      input_buffer.pop(NULL);
      input_buffer.push('\r');
      input_buffer.push('\n');
      size_t i = 0;
      for (i = 0; !input_buffer.is_empty(); i++) {
        input_buffer.pop(&buf[i]);
        if (i == (buflen - 3)) {
          buf[buflen - 2] = '\r';
          buf[buflen - 1] = '\n';
          break;
        }
      }
      display.setCursor(0, 24);
      display.setTextSize(1);
      display.fillRect(0, 24, 128, 8, SSD1306_BLACK);
      display.print(buf);
      display.display();
      status_clear_timer = millis() + 3000;
      write_log(buf);

      if (app_status.is_passthrough_enabled) {
        write_with_clock(Serial, buf);
      }
    }
  }
}

void write_log(const char* msg) {
  if (!MassStorageHost.mounted()) { return; }

  MscFile log_file = MassStorageHost.open("/log.txt", O_CREAT | O_RDWR | O_APPEND);
  if (!log_file) { return; }

  write_with_clock(log_file, msg);
  log_file.close();
}

void write_with_clock(Stream& stream, const char* msg) {
  stream.printf("%02d:%02d:%02d\t", Clock.get_hour(), Clock.get_minute(), Clock.get_second());
  stream.printf("%s", msg);
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
    app_status.redraw_status = true;
  }
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
  status_clear_timer = millis() + 3000;
}

void handle_serial() {
  if (Serial.available() == 0) { return; }
  String input = Serial.readStringUntil('\n');
  input.trim();
  if (input.equals("status")) {
    Serial.println("+OK");
    if (!KeyboardHost.mounted()) {
        Serial.printf("HID: unmounted%s\r\n");
    } else {
        Serial.printf("HID: mounted(0x%04x,0x%04x)\r\n", app_status.hid_vid, app_status.hid_pid);
    }
    if (!MassStorageHost.mounted()) {
        Serial.printf("MSC: unmounted%s\r\n");
    } else {
        Serial.printf("MSC: mounted(0x%04x,0x%04x)\r\n", app_status.msc_vid, app_status.msc_pid);
    }
    {
        uint8_t h, m, s;
        Clock.get_clock(&h, &m, &s);
        Serial.printf("Clock: %02d:%02d:%02d\r\n", h, m, s);
    }
    Serial.println();
    return;

  } else if (input.startsWith("read ")) {
    int offset, length;
    sscanf(input.c_str(), "read %d,%d", &offset, &length);
    if (length > 256) {
      Serial.println("-ERR: length should be equal or less than 256");
      Serial.println();
      return;
    }
    MscFile file = MassStorageHost.open("/log.txt", O_RDONLY);
    if (!file) {
      Serial.println("-ERR: file not found");
      Serial.println();
      return;
    }
    file.seek(offset);
    uint8_t buf[length] = {};
    size_t readlen = file.read(buf, length);
    file.close();
    Serial.printf("+OK: length=%u\r\n", readlen);
    Serial.write(buf, readlen);
    Serial.println();
    Serial.println();

  } else if (input.startsWith("clock set ")) {
    unsigned int h = 9, m = 0, s = 0;
    int num_of_fields = sscanf(input.c_str(), "clock set %u:%u:%u", &h, &m, &s);
    if (num_of_fields != 3) {
        Serial.println("-ERR: Invalid input.");
    } else {
        Clock.set_clock(h, m, s);
        Serial.printf("+OK: Clock is now %02d:%02d:%02d\r\n", h, m, s);
        Serial.println();
    }
    return;

  } else if (input.startsWith("dump set ")) {
      if (input.equals("dump set on")) {
          app_status.is_passthrough_enabled = true;
          Serial.println("+OK: passthrough enabled.");
      } else {
          app_status.is_passthrough_enabled = false;
          Serial.println("+OK: passthrough disabled.");
      }
      return;

  } else {
    Serial.println("-ERR: Unknown command.");
  }
}
