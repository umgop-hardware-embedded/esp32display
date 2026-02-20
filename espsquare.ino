/*
 * WiFi JPEG Image Viewer — Waveshare ESP32-S3-Touch-LCD-3.5B
 *
 * Root-cause fix for "random static" display:
 *   Arduino_GFX's QSPI writeRepeat/writePixels use addr=0x003C00 (RAMWRC)
 *   for the FIRST write.  The AXS15231B silently ignores RAMWRC without a
 *   preceding RAMWR in the same QIO transaction, so GRAM is never updated.
 *   LilyGO's reference implementation (T-Display-S3-Long) uses addr=0x002C00
 *   (RAMWR) for the first transaction, which is what this sketch does.
 *
 * This sketch bypasses Arduino_GFX entirely and uses the ESP-IDF SPI API
 * directly (spi_device_polling_transmit), matching the LilyGO approach.
 *
 * Board settings in Arduino IDE:
 *   Board      : ESP32S3 Dev Module
 *   PSRAM      : OPI PSRAM
 *   Flash Mode : QIO 80 MHz
 *   Partition  : Huge APP (3 MB No OTA / 1 MB SPIFFS)
 *   USB CDC    : Enabled
 *
 * Required library: JPEGDEC by bitbank2
 *   Arduino IDE → Library Manager → search "JPEGDEC" → Install
 *
 * Usage: fill in WIFI_SSID / WIFI_PASSWORD below, flash, open Serial at
 * 115200, then send '0'–'4' to switch between images or press the BOOT
 * button (GPIO 0) to cycle.
 */

#include <Arduino.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>

// ---- WiFi credentials — edit these ----
#define WIFI_SSID     "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_PASSWORD"

// ---- Image URLs (480×320 landscape JPEGs from picsum.photos) ----
#define NUM_IMAGES 5
static const char *imageUrls[NUM_IMAGES] = {
  "https://picsum.photos/id/1/480/320",    // 0
  "https://picsum.photos/id/10/480/320",   // 1
  "https://picsum.photos/id/20/480/320",   // 2
  "https://picsum.photos/id/30/480/320",   // 3
  "https://picsum.photos/id/40/480/320",   // 4
};

// ---- Pin definitions (Waveshare ESP32-S3-Touch-LCD-3.5B) ----
#define LCD_CS   39
#define LCD_CLK  40
#define LCD_D0   41   // DATA0 / MOSI
#define LCD_D1   42   // DATA1 / MISO
#define LCD_D2   45   // DATA2 / QUADWP
#define LCD_D3   46   // DATA3 / QUADHD
#define LCD_BL_A  6   // backlight candidate A (original probe)
#define LCD_BL_B  48  // backlight candidate B (Waveshare wiki)
#define LCD_RST  38
#define BOOT_PIN  0

// ---- Display geometry (landscape after MADCTL MX|MV) ----
#define SCREEN_W  480
#define SCREEN_H  320

// ---- SPI ----
#define LCD_SPI_HOST  SPI2_HOST
#define LCD_SPI_FREQ  (20 * 1000 * 1000)   // 20 MHz — safe for most AXS15231B panels
// One row of pixels per DMA chunk; fits in 960 bytes.
#define LCD_ROW_BYTES (SCREEN_W * 2)

static spi_device_handle_t lcd_spi;

// DMA-accessible pixel staging buffer (internal SRAM, DRAM_ATTR)
static DRAM_ATTR uint8_t  lcd_px_buf[LCD_ROW_BYTES];
// DMA-accessible command data buffer
static DRAM_ATTR uint8_t  lcd_cmd_buf[32];

// ============================================================
// Low-level SPI helpers
// ============================================================

static inline void cs_low()  { gpio_set_level((gpio_num_t)LCD_CS, 0); }
static inline void cs_high() { gpio_set_level((gpio_num_t)LCD_CS, 1); }

// Send one register command + optional data bytes via opcode 0x02 (config path).
// data may be in flash (const); it is copied to DRAM before the DMA transfer.
static void lcd_cmd(uint8_t reg, const uint8_t *data, uint8_t len) {
  spi_transaction_t t = {};
  t.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  t.cmd   = 0x02;
  t.addr  = (uint32_t)reg << 8;
  if (len > 0 && data) {
    uint8_t n = (len > sizeof(lcd_cmd_buf)) ? (uint8_t)sizeof(lcd_cmd_buf) : len;
    memcpy(lcd_cmd_buf, data, n);
    t.tx_buffer = lcd_cmd_buf;
    t.length    = (uint32_t)n * 8;
  }
  cs_low();
  spi_device_polling_transmit(lcd_spi, &t);
  cs_high();
}

// ============================================================
// Pixel write — THE critical fix
// ============================================================
//
// The first QIO transaction uses addr=0x002C00 (RAMWR = 0x2C).
// Subsequent chunks use addr=0x003C00 (RAMWRC) while CS stays asserted.
// Both use opcode 0x32 with SPI_TRANS_MODE_QIO so all four data lines carry
// pixel data — the only mode the AXS15231B accepts for bulk pixel writes.
//
// data  : big-endian RGB565 bytes (high byte first in memory)
// bytes : total byte count (must be even)

static void lcd_push_be(const uint8_t *data, uint32_t bytes) {
  bool first = true;
  cs_low();
  while (bytes > 0) {
    uint32_t chunk = (bytes > sizeof(lcd_px_buf)) ? sizeof(lcd_px_buf) : bytes;
    memcpy(lcd_px_buf, data, chunk);   // copy to DMA-safe SRAM

    spi_transaction_ext_t t = {};
    if (first) {
      t.base.flags = SPI_TRANS_MODE_QIO;
      t.base.cmd   = 0x32;
      t.base.addr  = 0x002C00;   // RAMWR — initiate pixel write
      first = false;
    } else {
      // No cmd/addr re-sent; CS stays low so the chip continues the write.
      t.base.flags       = SPI_TRANS_MODE_QIO
                         | SPI_TRANS_VARIABLE_CMD
                         | SPI_TRANS_VARIABLE_ADDR
                         | SPI_TRANS_VARIABLE_DUMMY;
      t.command_bits = 0;
      t.address_bits = 0;
      t.dummy_bits   = 0;
    }
    t.base.tx_buffer = lcd_px_buf;
    t.base.length    = chunk * 8;
    spi_device_polling_transmit(lcd_spi, (spi_transaction_t *)&t);
    data  += chunk;
    bytes -= chunk;
  }
  cs_high();
}

// Set CASET / RASET address window.
static void lcd_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
  uint8_t ca[4] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
  uint8_t ra[4] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2};
  lcd_cmd(0x2A, ca, 4);   // CASET
  lcd_cmd(0x2B, ra, 4);   // RASET
}

// Fill entire screen with a solid colour (big-endian RGB565).
static void lcd_fill(uint16_t color) {
  uint8_t hi = color >> 8, lo = color & 0xFF;
  for (uint32_t i = 0; i < sizeof(lcd_px_buf); i += 2) {
    lcd_px_buf[i]     = hi;
    lcd_px_buf[i + 1] = lo;
  }
  lcd_window(0, 0, SCREEN_W - 1, SCREEN_H - 1);
  uint32_t remaining = (uint32_t)SCREEN_W * SCREEN_H;   // pixels
  bool first = true;
  cs_low();
  while (remaining > 0) {
    uint32_t chunk_px = (remaining > sizeof(lcd_px_buf) / 2)
                        ? sizeof(lcd_px_buf) / 2 : remaining;
    spi_transaction_ext_t t = {};
    if (first) {
      t.base.flags = SPI_TRANS_MODE_QIO;
      t.base.cmd   = 0x32;
      t.base.addr  = 0x002C00;
      first = false;
    } else {
      t.base.flags       = SPI_TRANS_MODE_QIO
                         | SPI_TRANS_VARIABLE_CMD
                         | SPI_TRANS_VARIABLE_ADDR
                         | SPI_TRANS_VARIABLE_DUMMY;
      t.command_bits = 0;
      t.address_bits = 0;
      t.dummy_bits   = 0;
    }
    t.base.tx_buffer = lcd_px_buf;
    t.base.length    = chunk_px * 2 * 8;
    spi_device_polling_transmit(lcd_spi, (spi_transaction_t *)&t);
    remaining -= chunk_px;
  }
  cs_high();
}

// ============================================================
// SPI bus initialisation
// ============================================================
static bool lcd_spi_init() {
  gpio_set_direction((gpio_num_t)LCD_CS, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)LCD_CS, 1);

  spi_bus_config_t bus = {};
  bus.mosi_io_num   = LCD_D0;
  bus.miso_io_num   = LCD_D1;
  bus.sclk_io_num   = LCD_CLK;
  bus.quadwp_io_num = LCD_D2;
  bus.quadhd_io_num = LCD_D3;
  bus.max_transfer_sz = sizeof(lcd_px_buf) + 8;
  bus.flags           = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;

  esp_err_t r = spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
  if (r != ESP_OK) { Serial.printf("spi_bus_initialize err %d\n", r); return false; }

  spi_device_interface_config_t dev = {};
  dev.command_bits   = 8;
  dev.address_bits   = 24;
  dev.dummy_bits     = 0;
  dev.mode           = 0;           // SPI mode 0
  dev.clock_speed_hz = LCD_SPI_FREQ;
  dev.spics_io_num   = -1;          // CS managed manually
  dev.flags          = SPI_DEVICE_HALFDUPLEX;
  dev.queue_size     = 1;

  r = spi_bus_add_device(LCD_SPI_HOST, &dev, &lcd_spi);
  if (r != ESP_OK) { Serial.printf("spi_bus_add_device err %d\n", r); return false; }
  return true;
}

// ============================================================
// AXS15231B init sequence
// (matches axs15231b_320480_type2 from Arduino_GFX, confirmed working)
// ============================================================
static void lcd_init_display() {
  // Hardware reset
  pinMode(LCD_RST, OUTPUT);
  digitalWrite(LCD_RST, HIGH); delay(10);
  digitalWrite(LCD_RST, LOW);  delay(130);
  digitalWrite(LCD_RST, HIGH); delay(300);

  lcd_cmd(0x28, nullptr, 0); delay(20);   // display off
  lcd_cmd(0x10, nullptr, 0);              // sleep in

  static const uint8_t bb_unlock[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x5a,0xa5};
  lcd_cmd(0xBB, bb_unlock, 8);

  static const uint8_t a0[] = {0x00,0x30,0x00,0x02,0x00,0x00,0x05,0x3F,0x30,0x05,0x3F,0x3F,0x00,0x00,0x00,0x00,0x00};
  lcd_cmd(0xA0, a0, 17);

  // 0xA2 is 31 bytes — send in two lcd_cmd calls since lcd_cmd_buf is 32 bytes
  static const uint8_t a2[] = {0x30,0x04,0x14,0x50,0x80,0x30,0x85,0x80,0xB4,0x28,0xFF,0xFF,0xFF,0x20,0x50,0x10,0x02,0x06,0x20,0xD0,0xC0,0x01,0x12,0xA0,0x91,0xC0,0x20,0x7F,0xFF,0x00,0x06};
  lcd_cmd(0xA2, a2, 31);

  static const uint8_t d0[] = {0x80,0xb4,0x21,0x24,0x08,0x05,0x10,0x01,0xf2,0x02,0xc2,0x02,0x22,0x22,0xaa,0x03,0x10,0x12,0xc0,0x10,0x10,0x40,0x04,0x00,0x30,0x10,0x00,0x03,0x0d,0x12};
  lcd_cmd(0xD0, d0, 30);

  static const uint8_t a3[] = {0xA0,0x06,0xAA,0x00,0x08,0x02,0x0A,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x00,0x55,0x55};
  lcd_cmd(0xA3, a3, 22);

  static const uint8_t c1[] = {0x33,0x04,0x02,0x02,0x71,0x05,0x24,0x55,0x02,0x00,0x01,0x01,0x53,0xff,0xff,0xff,0x4f,0x52,0x00,0x4f,0x52,0x00,0x45,0x3b,0x0b,0x04,0x0d,0x00,0xff,0x42};
  lcd_cmd(0xC1, c1, 30);

  static const uint8_t c4[] = {0x00,0x24,0x33,0x80,0x66,0xea,0x64,0x32,0xC8,0x64,0xC8,0x32,0x90,0x90,0x11,0x06,0xDC,0xFA,0x00,0x00,0x80,0xFE,0x10,0x10,0x00,0x0A,0x0A,0x44,0x50};
  lcd_cmd(0xC4, c4, 29);

  static const uint8_t c5[] = {0x18,0x00,0x00,0x03,0xFE,0xe8,0x3b,0x20,0x30,0x10,0x88,0xde,0x0d,0x08,0x0f,0x0f,0x01,0xe8,0x3b,0x20,0x10,0x10,0x00};
  lcd_cmd(0xC5, c5, 23);

  static const uint8_t c6[] = {0x05,0x0A,0x05,0x0A,0x00,0xE0,0x2E,0x0B,0x12,0x22,0x12,0x22,0x01,0x03,0x00,0x02,0x6A,0x18,0xC8,0x22};
  lcd_cmd(0xC6, c6, 20);

  static const uint8_t c7[] = {0x50,0x36,0x28,0x00,0xa2,0x80,0x8f,0x00,0x80,0xff,0x07,0x11,0x9c,0x6f,0xff,0x24,0x0c,0x0d,0x0e,0x0f};
  lcd_cmd(0xC7, c7, 20);

  static const uint8_t c9[] = {0x33,0x44,0x44,0x01};
  lcd_cmd(0xC9, c9, 4);

  static const uint8_t cf[] = {0x2c,0x1e,0x88,0x58,0x13,0x18,0x56,0x18,0x1e,0x68,0xf7,0x00,0x66,0x0d,0x22,0xc4,0x0c,0x77,0x22,0x44,0xaa,0x55,0x04,0x04,0x12,0xa0,0x08};
  lcd_cmd(0xCF, cf, 27);

  static const uint8_t d5[] = {0x30,0x30,0x8a,0x00,0x44,0x04,0x4a,0xe5,0x02,0x4a,0xe5,0x02,0x04,0xd9,0x02,0x47,0x03,0x03,0x03,0x03,0x83,0x00,0x00,0x00,0x80,0x52,0x53,0x50,0x50,0x00};
  lcd_cmd(0xD5, d5, 30);

  static const uint8_t d6[] = {0x10,0x32,0x54,0x76,0x98,0xba,0xdc,0xfe,0x34,0x02,0x01,0x83,0xff,0x00,0x20,0x50,0x00,0x30,0x03,0x03,0x50,0x13,0x00,0x00,0x00,0x04,0x50,0x20,0x01,0x00};
  lcd_cmd(0xD6, d6, 30);

  static const uint8_t d7[] = {0x03,0x01,0x09,0x0b,0x0d,0x0f,0x1e,0x1f,0x18,0x1d,0x1f,0x19,0x30,0x30,0x04,0x00,0x20,0x20,0x1f};
  lcd_cmd(0xD7, d7, 19);

  static const uint8_t d8[] = {0x02,0x00,0x08,0x0a,0x0c,0x0e,0x1e,0x1f,0x18,0x1d,0x1f,0x19};
  lcd_cmd(0xD8, d8, 12);

  static const uint8_t df[] = {0x44,0x33,0x4b,0x69,0x00,0x0a,0x02,0x90};
  lcd_cmd(0xDF, df, 8);

  static const uint8_t e0[] = {0x1f,0x20,0x10,0x17,0x0d,0x09,0x12,0x2a,0x44,0x25,0x0c,0x15,0x13,0x31,0x36,0x2f,0x02};
  lcd_cmd(0xE0, e0, 17);
  static const uint8_t e1[] = {0x3f,0x20,0x10,0x16,0x0c,0x08,0x12,0x29,0x43,0x25,0x0c,0x15,0x13,0x32,0x36,0x2f,0x27};
  lcd_cmd(0xE1, e1, 17);
  static const uint8_t e2[] = {0x3B,0x07,0x12,0x18,0x0E,0x0D,0x17,0x35,0x44,0x32,0x0C,0x14,0x14,0x36,0x3A,0x2F,0x0D};
  lcd_cmd(0xE2, e2, 17);
  static const uint8_t e3[] = {0x37,0x07,0x12,0x18,0x0E,0x0D,0x17,0x35,0x44,0x32,0x0C,0x14,0x14,0x36,0x32,0x2F,0x0F};
  lcd_cmd(0xE3, e3, 17);
  static const uint8_t e4[] = {0x3B,0x07,0x12,0x18,0x0E,0x0D,0x17,0x39,0x44,0x2E,0x0C,0x14,0x14,0x36,0x3A,0x2F,0x0D};
  lcd_cmd(0xE4, e4, 17);
  static const uint8_t e5[] = {0x37,0x07,0x12,0x18,0x0E,0x0D,0x17,0x39,0x44,0x2E,0x0C,0x14,0x14,0x36,0x3A,0x2F,0x0F};
  lcd_cmd(0xE5, e5, 17);

  static const uint8_t bb_lock[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
  lcd_cmd(0xBB, bb_lock, 8);   // lock registers

  lcd_cmd(0x20, nullptr, 0);   // inversion off
  static const uint8_t colmod[] = {0x55};
  lcd_cmd(0x3A, colmod, 1);    // COLMOD: 16-bit RGB565

  lcd_cmd(0x11, nullptr, 0); delay(200);  // sleep out
  lcd_cmd(0x29, nullptr, 0); delay(100);  // display on

  // MADCTL: landscape — MX (0x40) | MV (0x20), RGB order
  static const uint8_t madctl[] = {0x60};
  lcd_cmd(0x36, madctl, 1);

  Serial.println("LCD init done");
}

// ============================================================
// JPEG decoder
// ============================================================
static JPEGDEC jpeg;

// JPEGDEC calls this for each decoded MCU block.
// pDraw->pPixels is big-endian RGB565 (RGB565_BIG_ENDIAN mode).
static int jpegCb(JPEGDRAW *pDraw) {
  lcd_window(pDraw->x, pDraw->y,
             pDraw->x + pDraw->iWidth  - 1,
             pDraw->y + pDraw->iHeight - 1);
  lcd_push_be((const uint8_t *)pDraw->pPixels,
              (uint32_t)pDraw->iWidth * pDraw->iHeight * 2);
  return 1;
}

// ============================================================
// State
// ============================================================
static int      currentImage = 0;
static bool     lastBoot     = true;
static uint32_t lastDebounce = 0;

// ============================================================
// Image fetch + display
// ============================================================
static void showImage(int idx) {
  if (idx < 0 || idx >= NUM_IMAGES) return;
  currentImage = idx;

  const char *url = imageUrls[idx];
  Serial.printf("[%d] %s\n", idx, url);
  lcd_fill(0x0000);   // clear to black while loading

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP error %d\n", code);
    http.end();
    return;
  }

  int contentLen = http.getSize();
  size_t bufSize = (contentLen > 0) ? (size_t)contentLen : 300000u;

  // Prefer PSRAM for the download buffer; fall back to heap
  uint8_t *buf = (uint8_t *)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t *)malloc(bufSize);
  if (!buf) {
    Serial.println("ERROR: out of memory for image buffer");
    http.end();
    return;
  }

  // Stream body into buffer
  WiFiClient *stream = http.getStreamPtr();
  size_t total = 0;
  uint32_t deadline = millis() + 15000;
  while (millis() < deadline && total < bufSize) {
    size_t avail = stream->available();
    if (avail > 0) {
      total += stream->readBytes(buf + total, min(avail, bufSize - total));
    } else if (!stream->connected()) {
      break;
    } else {
      delay(1);
    }
    if (contentLen > 0 && total >= (size_t)contentLen) break;
  }
  http.end();
  Serial.printf("Downloaded %u bytes\n", (unsigned)total);

  if (total == 0) { free(buf); return; }

  // Decode JPEG and push each MCU block directly to the display
  if (jpeg.openRAM(buf, (int)total, jpegCb)) {
    jpeg.setPixelType(RGB565_BIG_ENDIAN);   // output big-endian → lcd_push_be sends as-is
    jpeg.decode(0, 0, JPEG_AUTO_ROTATE);
    jpeg.close();
    Serial.println("OK");
  } else {
    Serial.println("JPEG decode failed");
  }
  free(buf);
}

// ============================================================
// Input
// ============================================================
static void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c >= '0' && c < '0' + NUM_IMAGES) showImage(c - '0');
  }
}

static void pollBoot() {
  bool b = digitalRead(BOOT_PIN);
  if (b != lastBoot) lastDebounce = millis();
  if (millis() - lastDebounce > 40 && lastBoot && !b)
    showImage((currentImage + 1) % NUM_IMAGES);
  lastBoot = b;
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("=== AXS15231B direct-SPI image viewer ===");

  // Backlight — drive both candidates; only the real pin has effect
  pinMode(LCD_BL_A, OUTPUT); digitalWrite(LCD_BL_A, HIGH);
  pinMode(LCD_BL_B, OUTPUT); digitalWrite(LCD_BL_B, HIGH);

  if (!lcd_spi_init()) {
    Serial.println("FATAL: SPI init failed");
    while (true) delay(1000);
  }
  Serial.println("SPI OK");

  lcd_init_display();

  // Colour flash — confirms pixel writes are reaching the GRAM
  lcd_fill(0xF800); delay(500);   // RED
  lcd_fill(0x07E0); delay(500);   // GREEN
  lcd_fill(0x001F); delay(500);   // BLUE
  Serial.println("Colour flash done — display is working if you saw R/G/B");

  // WiFi
  Serial.printf("Connecting to %s", WIFI_SSID);
  lcd_fill(0x0000);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500); Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi failed — check WIFI_SSID / WIFI_PASSWORD");
    delay(10000);
    ESP.restart();
  }
  Serial.printf("WiFi OK  IP: %s\n", WiFi.localIP().toString().c_str());

  pinMode(BOOT_PIN, INPUT_PULLUP);
  Serial.println("Send 0-4 to switch images | BOOT button cycles");

  showImage(0);
}

void loop() {
  pollSerial();
  pollBoot();
  delay(20);
}
