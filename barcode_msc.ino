/** Required libraries:
 * - Adafruit GFX
 * - Adafruit SSD1306
 */

#include <Adafruit_TinyUSB.h>
#include <KeyboardHost.h>
#include <MassStorageHost.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "textbuffer.h"
#include "clock_manager.h"
#include "ringbuffer.h"


constexpr int PIN_LED_1 = PB15;
constexpr int PIN_LED_2 = PA8;
constexpr int PIN_LED_3 = PA9;
constexpr int PIN_LED_4 = PA10;

constexpr int PIN_BTN_1 = PB12;
constexpr int PIN_BTN_2 = PB13;
constexpr int PIN_BTN_3 = PB14;

constexpr bool ENABLE_DEBUG_SERIAL = true;
constexpr unsigned long DEBUG_SERIAL_WAIT_INTERVAL_MS = 10;

#define DEBUG_LOG(msg) do { if (ENABLE_DEBUG_SERIAL && Serial) { Serial.println(msg); Serial.flush(); } } while (0)
#define DEBUG_LOGF(...) do { if (ENABLE_DEBUG_SERIAL && Serial) { Serial.printf(__VA_ARGS__); Serial.print("\r\n"); Serial.flush(); } } while (0)


#define SCREEN_ADDRESS 0x3C
// Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET)
Adafruit_SSD1306 display(128, 32, &Wire, -1);

Adafruit_USBH_Host USBHost;

Adafruit_USBD_MIDI USB_MIDI;

template<size_t in_len, size_t out_len>
class MIDIStream : public Stream {
public:
    RingBuffer<uint8_t, in_len>& inbuffer;
    TextBuffer<out_len>& outbuffer;

    MIDIStream(RingBuffer<uint8_t, in_len>& inbuffer, TextBuffer<out_len>& outbuffer) : 
            inbuffer(inbuffer), outbuffer(outbuffer) {

    }

    void begin() {
        inbuffer.begin();
        outbuffer.begin();
    }
    
    size_t write(uint8_t ch) override {
        if (outbuffer.is_full()) {
            flush();
        }
        return outbuffer.append_char(ch) ? 1 : 0;
    }
    
    size_t write(const uint8_t *buffer, size_t size) override {
        size_t cnt = 0;
        for (; cnt < size; ++cnt) {
            if (outbuffer.is_full()) {
                flush();
            }
            if (!outbuffer.append_char(buffer[cnt])) {
                break;
            }
        }
        return cnt;
    }
    
    int availableForWrite() override {
        return outbuffer.capacity() - outbuffer.length();
    }

    void convertHex(uint8_t val, char* dest) {
        uint8_t upper = val >> 4;
        uint8_t lower = val & 0x0f;
        dest[0] = (upper >= 0x0a) ? 'a' + (upper - 0x0a) : '0' + upper;
        dest[1] = (lower >= 0x0a) ? 'a' + (lower - 0x0a) : '0' + lower;
    }

    void flush() override {
        if (outbuffer.length() == 0) { return; }
        DEBUG_LOGF("[MIDI TX] flush len=%u mounted=%d text=\"%s\"", outbuffer.length(), TinyUSBDevice.mounted() ? 1 : 0, outbuffer.get_str());

        uint8_t packet[4] = {0x04, 0, 0, 0};
        uint8_t packet_len = 0;
        auto push_sysex_byte = [&](uint8_t b) {
            packet[1 + packet_len] = b;
            packet_len += 1;
            if (packet_len == 3) {
                packet[0] = 0x04;
                while (!USB_MIDI.writePacket(packet)) {
                    delay(1);
                }
                packet_len = 0;
            }
        };

        push_sysex_byte(0xF0);
        for (size_t i = 0; i < outbuffer.length(); i++) {
            char hex[2];
            convertHex(outbuffer.get_str()[i], hex);
            push_sysex_byte((uint8_t)hex[0]);
            push_sysex_byte((uint8_t)hex[1]);
        }
        push_sysex_byte(0xF7);

        if (packet_len > 0) {
            packet[0] = 0x04 + packet_len;
            for (uint8_t i = packet_len; i < 3; i++) {
                packet[1 + i] = 0x00;
            }
            while (!USB_MIDI.writePacket(packet)) {
                delay(1);
            }
        }

        outbuffer.clear();
    }

    int available() override {
        return inbuffer.available();
    }

    int read() override {
        uint8_t val = 0x00;
        if (inbuffer.pop_front(&val)) {
            return val;
        } else {
            return -1;
        }
    }
    
    int peek() override {
        uint8_t val = 0x00;
        if (inbuffer.peek_front(&val)) {
            return val;
        } else {
            return -1;
        }
    }

    size_t readBytes(uint8_t* buffer, size_t length) {
        return readBytes((char*)buffer, length);
    }

    size_t readBytes(char* buffer, size_t length) {
        size_t i = 0;
        for (; i < length; i++) {
            uint8_t val = 0;
            if (!inbuffer.pop_front(&val)) {
                return i;
            }
            buffer[i] = (char)val;
        }
        return i;
    }
};

// TextBuffer<64> midi_input_buffer;
RingBuffer<uint8_t, 64> midi_input_buffer;
TextBuffer<128> midi_output_buffer;
static MIDIStream<64, 128> MIDI_Stream(midi_input_buffer, midi_output_buffer);


typedef enum {
    TRANSPORT_CDC,
    TRANSPORT_MIDI
} transport_t;


typedef struct {
    transport_t transport;

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


static TextBuffer<256> textBuffer;
static TextBuffer<128> input_buffer;


ClockManager Clock;


static uint8_t fromHexChar(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

void wait_for_debug_serial() {
    if (!ENABLE_DEBUG_SERIAL) { return; }
    while (!Serial) {
        delay(DEBUG_SERIAL_WAIT_INTERVAL_MS);
    }
}

static char midi_rx_hex_buffer[128] = {};
static size_t midi_rx_hex_length = 0;

void decode_midi_sysex_buffer() {
    if ((midi_rx_hex_length & 1) != 0) {
        DEBUG_LOGF("[MIDI RX] odd hex length=%u", midi_rx_hex_length);
        midi_rx_hex_length = 0;
        midi_rx_hex_buffer[0] = '\0';
        return;
    }

    for (size_t i = 0; i + 1 < midi_rx_hex_length; i += 2) {
        const uint8_t b = (fromHexChar(midi_rx_hex_buffer[i]) << 4) | fromHexChar(midi_rx_hex_buffer[i + 1]);
        if (!midi_input_buffer.push_back(b)) {
            DEBUG_LOG("[MIDI RX] input buffer overflow");
            break;
        }
    }

    DEBUG_LOGF("[MIDI RX] decoded bytes=%u", midi_input_buffer.available());
    midi_rx_hex_length = 0;
    midi_rx_hex_buffer[0] = '\0';
}

void handle_midi_byte(uint8_t b) {
    if (b == 0xF0) {
        static bool led = false;
        led = !led;
        digitalWrite(PIN_LED_1, led ? HIGH : LOW);
        midi_rx_hex_length = 0;
        midi_rx_hex_buffer[0] = '\0';
        return;
    }

    if (b == 0xF7) {
        DEBUG_LOGF("[MIDI RX] SysEx size=%u", midi_rx_hex_length + 2);
        decode_midi_sysex_buffer();
        return;
    }

    if (midi_rx_hex_length + 1 >= sizeof(midi_rx_hex_buffer)) {
        DEBUG_LOG("[MIDI RX] hex buffer overflow");
        midi_rx_hex_length = 0;
        midi_rx_hex_buffer[0] = '\0';
        return;
    }

    midi_rx_hex_buffer[midi_rx_hex_length++] = (char)b;
    midi_rx_hex_buffer[midi_rx_hex_length] = '\0';
}

void poll_midi_rx() {
    uint8_t packet[4];
    while (USB_MIDI.readPacket(packet)) {
        DEBUG_LOGF("[MIDI RX] packet=%02x %02x %02x %02x", packet[0], packet[1], packet[2], packet[3]);
        const uint8_t cin = packet[0] & 0x0f;
        size_t payload_len = 0;
        if (cin == 0x04 || cin == 0x07) {
            payload_len = 3;
        } else if (cin == 0x06) {
            payload_len = 2;
        } else if (cin == 0x05) {
            payload_len = 1;
        } else {
            continue;
        }

        for (size_t i = 0; i < payload_len; i++) {
            handle_midi_byte(packet[1 + i]);
        }
    }
}


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

  if (digitalRead(PIN_BTN_2) == LOW) {
    app_status.transport = TRANSPORT_MIDI;
  } else {
    app_status.transport = TRANSPORT_CDC;
  }


  if (app_status.transport == TRANSPORT_CDC) {
    Serial.setStringDescriptor("HID Logger");
    TinyUSBDevice.addInterface(Serial);
    Serial.begin(115200);

    } else if (app_status.transport == TRANSPORT_MIDI) {
        if (ENABLE_DEBUG_SERIAL) {
            Serial.setStringDescriptor("HID Logger (Debug)");
            TinyUSBDevice.addInterface(Serial);
            Serial.begin(115200);
        }
        MIDI_Stream.begin();
        USB_MIDI.setStringDescriptor("HID Logger (MIDI)");
        USB_MIDI.begin();
    }

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

  if (app_status.transport == TRANSPORT_MIDI && ENABLE_DEBUG_SERIAL) {
    display.setTextSize(1);
    display.setCursor(0, 24);
    display.fillRect(0, 24, 128, 8, SSD1306_BLACK);
    display.print("Wait CDC...");
    display.display();
    wait_for_debug_serial();
    DEBUG_LOG("[BOOT] debug serial connected");
    DEBUG_LOGF("[BOOT] transport=%s", app_status.transport == TRANSPORT_MIDI ? "MIDI" : "CDC");
  }

  display.setTextSize(1);
  display.setCursor(0, 24);
  display.print("Ready.");
  display.display();
  request_clear_after_millis(3000);
}

void loop() {
  USBHost.task();
  poll_midi_rx();

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

  if (app_status.transport == TRANSPORT_CDC) {
      handle_comm(Serial);
  } else if (app_status.transport == TRANSPORT_MIDI) {
    handle_comm(MIDI_Stream);
    MIDI_Stream.flush();
  }

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
        if (app_status.transport == TRANSPORT_CDC) {
          write_with_clock(Serial, textBuffer.get_str());
          Serial.flush();
        } else if (app_status.transport == TRANSPORT_MIDI) {
          write_with_clock(MIDI_Stream, textBuffer.get_str());
          MIDI_Stream.flush();
        }
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

void handle_comm(Stream& stream) {
  while (stream.available() > 0) {
    char ch = (char)stream.read();
    DEBUG_LOGF("[COMM] read ch=0x%02x '%c'", (uint8_t)ch, (ch >= 0x20 && ch <= 0x7e) ? ch : '.');
    if (!input_buffer.append_char(ch)) {
        stream.println("-ERR: input too long");
        DEBUG_LOG("[COMM] input buffer overflow");
        input_buffer.clear();
        return;
    }

    if (ch != '\n') { continue; }
    input_buffer.trim_end();
    DEBUG_LOGF("[COMM] command=\"%s\"", input_buffer.get_str());

    if (input_buffer.equals("status")) {
        DEBUG_LOG("[COMM] status");
        stream.println("+OK");
        if (!KeyboardHost.mounted()) {
            stream.println("HID: unmounted");
        } else {
            stream.printf("HID: mounted(0x%04x,0x%04x)", app_status.hid_vid, app_status.hid_pid);
            stream.println();
        }
        if (!MassStorageHost.mounted()) {
            stream.println("MSC: unmounted");
        } else {
            stream.printf("MSC: mounted(0x%04x,0x%04x)", app_status.msc_vid, app_status.msc_pid);
            stream.println();
        }
        {
            uint8_t h, m, s;
            Clock.get_clock(&h, &m, &s);
            stream.printf("Clock: %02d:%02d:%02d", h, m, s);
            stream.println();
        }

    } else if (input_buffer.equals("log get size")) {
        DEBUG_LOG("[COMM] log get size");
        do {
        if (!MassStorageHost.mounted()) {
            stream.println("-ERR: Not mounted.");
            break;
        }
        MscFile file = MassStorageHost.open("/log.txt", O_RDONLY);
        if (!file) {
            stream.println("-ERR: log file not found.");
            break;
        }
            size_t filesize = file.size();
            file.close();
            stream.printf("+OK: filesize=%u", filesize);
            stream.println();
        } while (false);

    } else if (input_buffer.starts_with("log read ")) {
        DEBUG_LOG("[COMM] log read");
        do {
            uint32_t offset, length;
            sscanf(input_buffer.get_str(), "log read %u,%u", &offset, &length);
            MscFile file = MassStorageHost.open("/log.txt", O_RDONLY);
            if (!file) {
                stream.println("-ERR: file not found");
                stream.println();
                break;
            }
            size_t filesize = file.size();
            if (offset > filesize) {
                stream.println("-ERR: Invalid offset");
                file.close();
                return;
            }
            if (offset + length > filesize) {
                stream.println("-ERR: Invalid length");
                file.close();
                return;
            }
            file.seek(offset);
            stream.printf("+OK: offset=%d,length=%d", offset, length);
            stream.println();
            uint8_t buf[64];
            while (length > 0) {
                size_t chunksize = 63;
                if (chunksize > length) {
                chunksize = length;
                }
                size_t readlen = file.read(buf, chunksize);
                stream.write(buf, readlen);
                stream.flush();
                length -= readlen;
            }
            file.close();
            stream.println();
        } while (false);

    } else if (input_buffer.equals("log clear")) {
        DEBUG_LOG("[COMM] log clear");
        do {
            if (!MassStorageHost.mounted()) {
                stream.println("-ERR: Not mounted.");
                break;
            }
            MscFile file = MassStorageHost.open("/log.txt", O_CREAT | O_WRITE | O_TRUNC);
            if (!file) {
            stream.println("-ERR: log file not found.");
            break;
            }
            file.close();
            stream.println("+OK: log file cleared.");
        } while (false);

    } else if (input_buffer.starts_with("clock set ")) {
        DEBUG_LOG("[COMM] clock set");
        unsigned int h = 9, m = 0, s = 0;
        int num_of_fields = sscanf(input_buffer.get_str(), "clock set %u:%u:%u", &h, &m, &s);
        if (num_of_fields != 3) {
            stream.println("-ERR: Invalid input.");
        } else {
            Clock.set_clock(h, m, s);
            stream.printf("+OK: Clock is now %02d:%02d:%02d", h, m, s);
            stream.println();
        }

    } else if (input_buffer.starts_with("dump set ")) {
        DEBUG_LOG("[COMM] dump set");
        if (input_buffer.equals("dump set on")) {
            app_status.is_passthrough_enabled = true;
            stream.println("+OK: passthrough enabled.");
        } else {
            app_status.is_passthrough_enabled = false;
            stream.println("+OK: passthrough disabled.");
        }

    } else {
        DEBUG_LOG("[COMM] unknown command");
        stream.println("-ERR: Unknown command.");
    }

    input_buffer.clear();
    return;
  }
}




