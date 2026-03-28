/*
 * AXS15231B Pattern Viewer — Waveshare ESP32-S3-Touch-LCD-3.5B
 *
 * Send '0'–'4' over Serial (115200) to switch patterns.
 * Press BOOT button (GPIO 0) to cycle to the next pattern.
 *
 * Board settings (Arduino IDE → Tools):
 *   Board      : ESP32S3 Dev Module
 *   PSRAM      : OPI PSRAM
 *   Flash Mode : QIO 80 MHz
 *   Partition  : Huge APP (3 MB No OTA / 1 MB SPIFFS)
 *   USB CDC    : Enabled
 *
 * No extra libraries needed.
 *
 * ── Display fix ──────────────────────────────────────────────────────────────
 * Root cause of "random static": Arduino_GFX uses addr=0x003C00 (RAMWRC) for
 * the FIRST QIO pixel transaction.  The AXS15231B silently ignores RAMWRC
 * without a prior RAMWR in the same session, so GRAM is never written.
 *
 * This sketch uses the ESP-IDF SPI API directly.  Each pixel chunk is a
 * complete CS-asserted transaction:
 *   • first chunk  → cmd=0x32, addr=0x002C00 (RAMWR  – start write)
 *   • next chunks  → cmd=0x32, addr=0x003C00 (RAMWRC – continue)
 * CS deasserts and reasserts between every chunk (matches LilyGO reference).
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <Arduino.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"

// ── Pin definitions (Waveshare ESP32-S3-Touch-LCD-3.5B) ──────────────────────
#define LCD_CS    39
#define LCD_CLK   40
#define LCD_D0    41   // DATA0 / MOSI
#define LCD_D1    42   // DATA1 / MISO
#define LCD_D2    45   // DATA2 / WP
#define LCD_D3    46   // DATA3 / HD
#define LCD_BL_A   6   // backlight candidate A
#define LCD_BL_B  48   // backlight candidate B (Waveshare wiki)
#define LCD_RST   38
#define BOOT_PIN   0

// ── Display geometry (landscape after MADCTL 0x60) ───────────────────────────
#define SCREEN_W  480
#define SCREEN_H  320

// ── SPI ──────────────────────────────────────────────────────────────────────
#define LCD_SPI_HOST  SPI2_HOST
#define LCD_SPI_FREQ  (20 * 1000 * 1000)   // 20 MHz

static spi_device_handle_t lcd_spi;

// Pixel staging buffer — must stay in internal SRAM for DMA.
// One row of pixels (480 × 2 bytes = 960 bytes) fits comfortably.
static DRAM_ATTR uint8_t lcd_px_buf[SCREEN_W * 2];

// Small command-data buffer (also DRAM for DMA safety).
static DRAM_ATTR uint8_t lcd_cmd_buf[32];

// ── CS helpers ───────────────────────────────────────────────────────────────
static inline void cs_low()  { gpio_set_level((gpio_num_t)LCD_CS, 0); }
static inline void cs_high() { gpio_set_level((gpio_num_t)LCD_CS, 1); }

// ── Send one register command + optional data (opcode 0x02, single-line SPI) ─
static void lcd_cmd(uint8_t reg, const uint8_t *data, uint8_t len)
{
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

// ── Push big-endian RGB565 pixel bytes to the current address window ──────────
//
// THE fix: each chunk is its own complete CS-asserted transaction.
//   first chunk  → addr=0x002C00  (RAMWR  = 0x2C, start a new write)
//   later chunks → addr=0x003C00  (RAMWRC = 0x3C, continue the write)
// CS deasserts between chunks; the chip holds the GRAM pointer between them.
//
static void lcd_push_be(const uint8_t *data, uint32_t bytes)
{
    bool first = true;
    while (bytes > 0) {
        uint32_t chunk = (bytes > sizeof(lcd_px_buf)) ? sizeof(lcd_px_buf) : bytes;
        memcpy(lcd_px_buf, data, chunk);

        spi_transaction_t t = {};
        t.flags     = SPI_TRANS_MODE_QIO;
        t.cmd       = 0x32;
        t.addr      = first ? 0x002C00u : 0x003C00u;
        t.tx_buffer = lcd_px_buf;
        t.length    = chunk * 8;

        cs_low();
        spi_device_polling_transmit(lcd_spi, &t);
        cs_high();

        first  = false;
        data  += chunk;
        bytes -= chunk;
    }
}

// ── Set CASET / RASET address window ─────────────────────────────────────────
static void lcd_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t ca[4] = {(uint8_t)(x1>>8),(uint8_t)x1,(uint8_t)(x2>>8),(uint8_t)x2};
    uint8_t ra[4] = {(uint8_t)(y1>>8),(uint8_t)y1,(uint8_t)(y2>>8),(uint8_t)y2};
    lcd_cmd(0x2A, ca, 4);
    lcd_cmd(0x2B, ra, 4);
}

// ── Fill entire screen with one solid RGB565 colour ───────────────────────────
static void lcd_fill(uint16_t color)
{
    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (int i = 0; i < (int)sizeof(lcd_px_buf); i += 2) {
        lcd_px_buf[i]   = hi;
        lcd_px_buf[i+1] = lo;
    }
    lcd_window(0, 0, SCREEN_W-1, SCREEN_H-1);
    uint32_t remaining = (uint32_t)SCREEN_W * SCREEN_H;
    bool first = true;
    while (remaining > 0) {
        uint32_t chunk_px = (remaining > sizeof(lcd_px_buf)/2)
                            ? sizeof(lcd_px_buf)/2 : remaining;
        spi_transaction_t t = {};
        t.flags     = SPI_TRANS_MODE_QIO;
        t.cmd       = 0x32;
        t.addr      = first ? 0x002C00u : 0x003C00u;
        t.tx_buffer = lcd_px_buf;
        t.length    = chunk_px * 2 * 8;

        cs_low();
        spi_device_polling_transmit(lcd_spi, &t);
        cs_high();

        first      = false;
        remaining -= chunk_px;
    }
}

// ── SPI bus init ─────────────────────────────────────────────────────────────
static bool lcd_spi_init()
{
    gpio_set_direction((gpio_num_t)LCD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)LCD_CS, 1);

    spi_bus_config_t bus = {};
    bus.mosi_io_num   = LCD_D0;
    bus.miso_io_num   = LCD_D1;
    bus.sclk_io_num   = LCD_CLK;
    bus.quadwp_io_num = LCD_D2;
    bus.quadhd_io_num = LCD_D3;
    bus.max_transfer_sz = sizeof(lcd_px_buf) + 8;
    bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;

    esp_err_t r = spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (r != ESP_OK) { Serial.printf("spi_bus_initialize err %d\n", r); return false; }

    spi_device_interface_config_t dev = {};
    dev.command_bits   = 8;
    dev.address_bits   = 24;
    dev.dummy_bits     = 0;
    dev.mode           = 0;
    dev.clock_speed_hz = LCD_SPI_FREQ;
    dev.spics_io_num   = -1;   // CS managed manually
    dev.flags          = SPI_DEVICE_HALFDUPLEX;
    dev.queue_size     = 1;

    r = spi_bus_add_device(LCD_SPI_HOST, &dev, &lcd_spi);
    if (r != ESP_OK) { Serial.printf("spi_bus_add_device err %d\n", r); return false; }
    return true;
}

// ── AXS15231B init (type2, 320×480 portrait → landscape via MADCTL) ──────────
static void lcd_init_display()
{
    // Hardware reset
    pinMode(LCD_RST, OUTPUT);
    digitalWrite(LCD_RST, HIGH); delay(10);
    digitalWrite(LCD_RST, LOW);  delay(130);
    digitalWrite(LCD_RST, HIGH); delay(300);

    lcd_cmd(0x28, nullptr, 0); delay(20);
    lcd_cmd(0x10, nullptr, 0);

    static const uint8_t bb_unlock[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x5a,0xa5};
    lcd_cmd(0xBB, bb_unlock, 8);

    static const uint8_t a0[] = {0x00,0x30,0x00,0x02,0x00,0x00,0x05,0x3F,0x30,
                                   0x05,0x3F,0x3F,0x00,0x00,0x00,0x00,0x00};
    lcd_cmd(0xA0, a0, 17);

    static const uint8_t a2[] = {0x30,0x04,0x14,0x50,0x80,0x30,0x85,0x80,0xB4,
                                   0x28,0xFF,0xFF,0xFF,0x20,0x50,0x10,0x02,0x06,
                                   0x20,0xD0,0xC0,0x01,0x12,0xA0,0x91,0xC0,0x20,
                                   0x7F,0xFF,0x00,0x06};
    lcd_cmd(0xA2, a2, 31);

    static const uint8_t d0[] = {0x80,0xb4,0x21,0x24,0x08,0x05,0x10,0x01,0xf2,
                                   0x02,0xc2,0x02,0x22,0x22,0xaa,0x03,0x10,0x12,
                                   0xc0,0x10,0x10,0x40,0x04,0x00,0x30,0x10,0x00,
                                   0x03,0x0d,0x12};
    lcd_cmd(0xD0, d0, 30);

    static const uint8_t a3[] = {0xA0,0x06,0xAA,0x00,0x08,0x02,0x0A,0x04,0x04,
                                   0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,
                                   0x04,0x00,0x55,0x55};
    lcd_cmd(0xA3, a3, 22);

    static const uint8_t c1[] = {0x33,0x04,0x02,0x02,0x71,0x05,0x24,0x55,0x02,
                                   0x00,0x01,0x01,0x53,0xff,0xff,0xff,0x4f,0x52,
                                   0x00,0x4f,0x52,0x00,0x45,0x3b,0x0b,0x04,0x0d,
                                   0x00,0xff,0x42};
    lcd_cmd(0xC1, c1, 30);

    static const uint8_t c4[] = {0x00,0x24,0x33,0x80,0x66,0xea,0x64,0x32,0xC8,
                                   0x64,0xC8,0x32,0x90,0x90,0x11,0x06,0xDC,0xFA,
                                   0x00,0x00,0x80,0xFE,0x10,0x10,0x00,0x0A,0x0A,
                                   0x44,0x50};
    lcd_cmd(0xC4, c4, 29);

    static const uint8_t c5[] = {0x18,0x00,0x00,0x03,0xFE,0xe8,0x3b,0x20,0x30,
                                   0x10,0x88,0xde,0x0d,0x08,0x0f,0x0f,0x01,0xe8,
                                   0x3b,0x20,0x10,0x10,0x00};
    lcd_cmd(0xC5, c5, 23);

    static const uint8_t c6[] = {0x05,0x0A,0x05,0x0A,0x00,0xE0,0x2E,0x0B,0x12,
                                   0x22,0x12,0x22,0x01,0x03,0x00,0x02,0x6A,0x18,
                                   0xC8,0x22};
    lcd_cmd(0xC6, c6, 20);

    static const uint8_t c7[] = {0x50,0x36,0x28,0x00,0xa2,0x80,0x8f,0x00,0x80,
                                   0xff,0x07,0x11,0x9c,0x6f,0xff,0x24,0x0c,0x0d,
                                   0x0e,0x0f};
    lcd_cmd(0xC7, c7, 20);

    static const uint8_t c9[] = {0x33,0x44,0x44,0x01};
    lcd_cmd(0xC9, c9, 4);

    static const uint8_t cf[] = {0x2c,0x1e,0x88,0x58,0x13,0x18,0x56,0x18,0x1e,
                                   0x68,0xf7,0x00,0x66,0x0d,0x22,0xc4,0x0c,0x77,
                                   0x22,0x44,0xaa,0x55,0x04,0x04,0x12,0xa0,0x08};
    lcd_cmd(0xCF, cf, 27);

    static const uint8_t d5[] = {0x30,0x30,0x8a,0x00,0x44,0x04,0x4a,0xe5,0x02,
                                   0x4a,0xe5,0x02,0x04,0xd9,0x02,0x47,0x03,0x03,
                                   0x03,0x03,0x83,0x00,0x00,0x00,0x80,0x52,0x53,
                                   0x50,0x50,0x00};
    lcd_cmd(0xD5, d5, 30);

    static const uint8_t d6[] = {0x10,0x32,0x54,0x76,0x98,0xba,0xdc,0xfe,0x34,
                                   0x02,0x01,0x83,0xff,0x00,0x20,0x50,0x00,0x30,
                                   0x03,0x03,0x50,0x13,0x00,0x00,0x00,0x04,0x50,
                                   0x20,0x01,0x00};
    lcd_cmd(0xD6, d6, 30);

    static const uint8_t d7[] = {0x03,0x01,0x09,0x0b,0x0d,0x0f,0x1e,0x1f,0x18,
                                   0x1d,0x1f,0x19,0x30,0x30,0x04,0x00,0x20,0x20,
                                   0x1f};
    lcd_cmd(0xD7, d7, 19);

    static const uint8_t d8[] = {0x02,0x00,0x08,0x0a,0x0c,0x0e,0x1e,0x1f,0x18,
                                   0x1d,0x1f,0x19};
    lcd_cmd(0xD8, d8, 12);

    static const uint8_t df[] = {0x44,0x33,0x4b,0x69,0x00,0x0a,0x02,0x90};
    lcd_cmd(0xDF, df, 8);

    static const uint8_t e0[] = {0x1f,0x20,0x10,0x17,0x0d,0x09,0x12,0x2a,0x44,
                                   0x25,0x0c,0x15,0x13,0x31,0x36,0x2f,0x02};
    lcd_cmd(0xE0, e0, 17);
    static const uint8_t e1[] = {0x3f,0x20,0x10,0x16,0x0c,0x08,0x12,0x29,0x43,
                                   0x25,0x0c,0x15,0x13,0x32,0x36,0x2f,0x27};
    lcd_cmd(0xE1, e1, 17);
    static const uint8_t e2[] = {0x3B,0x07,0x12,0x18,0x0E,0x0D,0x17,0x35,0x44,
                                   0x32,0x0C,0x14,0x14,0x36,0x3A,0x2F,0x0D};
    lcd_cmd(0xE2, e2, 17);
    static const uint8_t e3[] = {0x37,0x07,0x12,0x18,0x0E,0x0D,0x17,0x35,0x44,
                                   0x32,0x0C,0x14,0x14,0x36,0x32,0x2F,0x0F};
    lcd_cmd(0xE3, e3, 17);
    static const uint8_t e4[] = {0x3B,0x07,0x12,0x18,0x0E,0x0D,0x17,0x39,0x44,
                                   0x2E,0x0C,0x14,0x14,0x36,0x3A,0x2F,0x0D};
    lcd_cmd(0xE4, e4, 17);
    static const uint8_t e5[] = {0x37,0x07,0x12,0x18,0x0E,0x0D,0x17,0x39,0x44,
                                   0x2E,0x0C,0x14,0x14,0x36,0x3A,0x2F,0x0F};
    lcd_cmd(0xE5, e5, 17);

    static const uint8_t bb_lock[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    lcd_cmd(0xBB, bb_lock, 8);

    lcd_cmd(0x20, nullptr, 0);             // display inversion off
    static const uint8_t colmod[] = {0x55};
    lcd_cmd(0x3A, colmod, 1);             // COLMOD: 16-bit RGB565

    lcd_cmd(0x11, nullptr, 0); delay(200); // sleep out
    lcd_cmd(0x29, nullptr, 0); delay(100); // display on

    // MADCTL 0x60 = MX|MV → landscape (480×320)
    static const uint8_t madctl[] = {0x60};
    lcd_cmd(0x36, madctl, 1);

    Serial.println("LCD init done");
}

// ── RGB565 colour helper ──────────────────────────────────────────────────────
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r & 0xF8) << 8)
         | ((uint16_t)(g & 0xFC) << 3)
         |  (uint16_t)(b >> 3);
}

// ── Push one row of SCREEN_W pixels (big-endian) to display ──────────────────
// row: 0-based y, pixels: array of SCREEN_W uint16_t in big-endian order
static void push_row(int row, const uint16_t *pixels)
{
    lcd_window(0, row, SCREEN_W-1, row);
    lcd_push_be((const uint8_t *)pixels, SCREEN_W * 2);
}

// ── Pattern 0: solid RED ──────────────────────────────────────────────────────
static void pattern0()
{
    lcd_fill(rgb(220, 30, 30));
    Serial.println("Pattern 0: Red");
}

// ── Pattern 1: solid GREEN ────────────────────────────────────────────────────
static void pattern1()
{
    lcd_fill(rgb(30, 220, 30));
    Serial.println("Pattern 1: Green");
}

// ── Pattern 2: solid BLUE ─────────────────────────────────────────────────────
static void pattern2()
{
    lcd_fill(rgb(30, 30, 220));
    Serial.println("Pattern 2: Blue");
}

// ── Pattern 3: horizontal colour bars (ROYGBIV + white + black) ───────────────
static void pattern3()
{
    static const uint16_t bars[] = {
        rgb(255,   0,   0),   // red
        rgb(255, 128,   0),   // orange
        rgb(255, 255,   0),   // yellow
        rgb(  0, 255,   0),   // green
        rgb(  0,   0, 255),   // blue
        rgb( 75,   0, 130),   // indigo
        rgb(148,   0, 211),   // violet
        rgb(255, 255, 255),   // white
        rgb(  0,   0,   0),   // black
    };
    const int nBars = sizeof(bars) / sizeof(bars[0]);

    uint16_t rowBuf[SCREEN_W];
    for (int y = 0; y < SCREEN_H; y++) {
        int barIdx = (y * nBars) / SCREEN_H;
        uint16_t c = bars[barIdx];
        // AXS15231B is big-endian: swap bytes
        uint16_t be = (c >> 8) | (c << 8);
        for (int x = 0; x < SCREEN_W; x++) rowBuf[x] = be;
        push_row(y, rowBuf);
    }
    Serial.println("Pattern 3: Colour bars");
}

// ── Pattern 4: RGB gradient (red→green left-to-right, brightness top-to-bottom)
static void pattern4()
{
    uint16_t rowBuf[SCREEN_W];
    for (int y = 0; y < SCREEN_H; y++) {
        uint8_t bright = (uint8_t)(255 - (y * 200) / SCREEN_H);
        for (int x = 0; x < SCREEN_W; x++) {
            uint8_t r = (uint8_t)((uint32_t)bright * (SCREEN_W - x) / SCREEN_W);
            uint8_t g = (uint8_t)((uint32_t)bright * x / SCREEN_W);
            uint8_t b = (uint8_t)(bright / 2);
            uint16_t c = rgb(r, g, b);
            rowBuf[x] = (c >> 8) | (c << 8);   // big-endian
        }
        push_row(y, rowBuf);
    }
    Serial.println("Pattern 4: Gradient");
}

// ── Scene dispatch ─────────────────────────────────────────────────────────────
static int currentPattern = 0;

static void showPattern(int idx)
{
    currentPattern = (idx % 5 + 5) % 5;
    Serial.printf("Showing pattern %d\n", currentPattern);
    switch (currentPattern) {
        case 0: pattern0(); break;
        case 1: pattern1(); break;
        case 2: pattern2(); break;
        case 3: pattern3(); break;
        case 4: pattern4(); break;
    }
}

// ── Input ──────────────────────────────────────────────────────────────────────
static bool     lastBoot     = true;
static uint32_t lastDebounce = 0;

static void pollSerial()
{
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c >= '0' && c <= '4') showPattern(c - '0');
    }
}

static void pollBoot()
{
    bool b = digitalRead(BOOT_PIN);
    if (b != lastBoot) lastDebounce = millis();
    if (millis() - lastDebounce > 40 && lastBoot && !b)
        showPattern(currentPattern + 1);
    lastBoot = b;
}

// ── setup / loop ───────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("=== AXS15231B Pattern Viewer ===");

    // Backlight — drive both candidates; only the wired pin has effect
    pinMode(LCD_BL_A, OUTPUT); digitalWrite(LCD_BL_A, HIGH);
    pinMode(LCD_BL_B, OUTPUT); digitalWrite(LCD_BL_B, HIGH);
    Serial.println("Backlight on (GPIO 6 + GPIO 48)");

    if (!lcd_spi_init()) {
        Serial.println("FATAL: SPI init failed — check wiring");
        while (true) delay(1000);
    }
    Serial.println("SPI OK");

    lcd_init_display();

    // Three-colour flash — confirms pixels reach the GRAM
    Serial.println("Colour flash starting...");
    lcd_fill(0xF800); delay(600);   // RED
    lcd_fill(0x07E0); delay(600);   // GREEN
    lcd_fill(0x001F); delay(600);   // BLUE
    Serial.println("Colour flash done");

    pinMode(BOOT_PIN, INPUT_PULLUP);

    Serial.println("Send 0-4 to switch patterns | BOOT button cycles");
    showPattern(0);
}

void loop()
{
    pollSerial();
    pollBoot();
    delay(20);
}
