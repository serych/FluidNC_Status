/**
 * @file main.c
 * @brief GRBL status LED indicator for ATtiny412 + NeoPixel.
 *
 * Parses FluidNC/GRBL status messages from USART0 and drives a small
 * RGB LED chain (NeoPixel) to visualize the current machine state.
 *
 * - Before a "[MSG:INFO: Connected" message is received, the LED blinks
 *   red <-> purple to indicate waiting-for-boot.
 * - After BOOTED, the LED color reflects current GRBL status (Idle, Run, etc.).
 * - If no status update is seen for a while, periodically requests status ("?\n").
 *
 * @note MCU: ATtiny412 (AVR-0/1 series)
 * @note LED: WS2812-compatible on PA3 (alternate USART on PA1/PA2)
 */

#define F_CPU 20000000UL

#ifndef USART0_BAUD_RATE
/// @brief Compute USART0.BAUD register value for a desired baud rate.
#define USART0_BAUD_RATE(BAUD_RATE) ((float)(F_CPU * 64 / (16 * (float)(BAUD_RATE))) + 0.5f)
#endif

//#define DEBUG 1

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include <tinyNeoPixel_Static.h>  // NeoPixel driver (uses global pixel buffer)

// ================== Hardware / Pins ==================
#define LED        PIN_PA3  ///< NeoPixel data pin.
#define TX         PIN_PA1  ///< USART TX (info only, pin config is done in uart_init()).
#define RX         PIN_PA2  ///< USART RX (info only).
#define BAUDRATE   115200   ///< USART baud rate.

#define NUM_LEDS   2        ///< Number of NeoPixels in the chain.
#define BRIGHTNESS 31       ///< Global NeoPixel brightness (0..255).

// ================== Colors ==================
#define COL_RED 0xff0000u  ///< Red
#define COL_ORA 0xff5f00u  ///< Orange
#define COL_YEL 0xffcf00u  ///< Yellow
#define COL_GRN 0x00ff00u  ///< Green
#define COL_CYA 0x007fffu  ///< Cyan
#define COL_PUR 0xff00ffu  ///< Purple (magenta)

// ================== Timings (ms) ==================
#define BLINK_INTERVAL      250u   ///< Startup blink period before BOOTED is seen.
#define REQUEST_TIMEOUT_MS  5000u  ///< If no status for this long, send "?\n" to FluidNC.

// ================== GRBL messages to parse ==================
#define MAX_PARSE_LEN 25  ///< Maximum parsed string length (prefix-only compare).
#define MSG_BOOTED "[MSG:INFO: Connected"
#define MSG_IDLE   "<Idle"
#define MSG_RUN    "<Run"
#define MSG_HOLD   "<Hold"
#define MSG_JOG    "<Jog"
#define MSG_DOOR   "<Door"
#define MSG_HOME   "<Home"
#define MSG_ALARM  "<Alarm"

/**
 * @enum Status
 * @brief Parsed GRBL states used to drive the LED.
 */
typedef enum Status {
  BOOTED,   ///< Controller reported boot/connect info.
  IDLE,     ///< "<Idle"
  RUN,      ///< "<Run"
  HOLD,     ///< "<Hold"
  JOG,      ///< "<Jog"
  DOOR,     ///< "<Door"
  HOME,     ///< "<Home"
  ALARM,    ///< "<Alarm"
  UNKNOWN = 255 ///< Not parsed or incomplete.
} Status;

// ================== NeoPixel ==================
byte pixels[NUM_LEDS * 3];
tinyNeoPixel leds = tinyNeoPixel(NUM_LEDS, LED, NEO_GRB + NEO_KHZ800, pixels);

// ================== Forward declarations (Arduino provides prototypes, but Doxygen likes these) ==================
static void uart_init(void);
static bool uart_available(void);
static uint8_t uart_read(void);
static void uart_write(uint8_t b);
static void uart_write_str(const char *str);
static void setColor(uint32_t color);
static void showStatus(Status st);
static Status parse_status(void);

#ifdef DEBUG
static void debugPrint(const char *buf);
#endif

// ================== USART0 (register-level) ==================

/**
 * @brief Initialize USART0 on alternate pins PA1 (TX) and PA2 (RX), 8N1, async.
 *
 * Configures the port mux for alternate USART pins, sets pin directions,
 * computes and sets the baud rate, and enables RX/TX.
 */
static void uart_init(void) {
  // Select alternate pins for USART0 (PA1 TX / PA2 RX)
  PORTMUX.CTRLB |= 1;

  // Directions
  PORTA.DIR &= ~PIN2_bm;  // RX as input
  PORTA.DIR |= PIN1_bm;   // TX as output

  // Baud
  USART0.BAUD = (uint16_t)USART0_BAUD_RATE(BAUDRATE);

  // Enable RX & TX
  USART0.CTRLB |= USART_RXEN_bm | USART_TXEN_bm;
}

/**
 * @brief Non-blocking check for a received byte.
 * @return true if a byte is waiting in RX buffer, false otherwise.
 */
static bool uart_available(void) {
  return (USART0.STATUS & USART_RXCIF_bm);
}

/**
 * @brief Read one byte from USART0.
 * @warning Call only if @ref uart_available returned true.
 * @return The received byte.
 */
static uint8_t uart_read(void) {
  return USART0.RXDATAL;
}

/**
 * @brief Write one byte to USART0 (blocking until data register empty).
 * @param b Byte to transmit.
 */
static void uart_write(uint8_t b) {
  while (!(USART0.STATUS & USART_DREIF_bm)) {
    /* wait */
  }
  USART0.TXDATAL = b;
}

/**
 * @brief Write a zero-terminated C string to USART0.
 * @param str Pointer to a null-terminated string.
 */
static void uart_write_str(const char *str) {
  for (size_t i = 0; i < strlen(str); i++) {
    uart_write((uint8_t)str[i]);
  }
}

// ================== LED helpers ==================

/**
 * @brief Set all NeoPixels to a 24-bit RGB color and show.
 * @param color 24-bit color as 0xRRGGBB.
 */
static void setColor(uint32_t color) {
  leds.fill(color, 0, NUM_LEDS);
  leds.show();
}

/**
 * @brief Display a color corresponding to a parsed GRBL status.
 * @param st Parsed status value.
 */
static void showStatus(Status st) {
  switch (st) {
    case BOOTED: setColor(COL_GRN); break;
    case IDLE:   setColor(COL_GRN); break;
    case RUN:    setColor(COL_CYA); break;
    case HOLD:   setColor(COL_YEL); break;
    case JOG:    setColor(COL_PUR); break;
    case DOOR:   setColor(COL_ORA); break;
    case HOME:   setColor(COL_PUR); break;
    case ALARM:  setColor(COL_RED); break;
    default:     /* no change */    break;
  }
}

// ================== Debug print ==================
#ifdef DEBUG
/**
 * @brief Send the beginning of a received line and its length to TX.
 * @param buf Null-terminated buffer containing the received line.
 */
static void debugPrint(const char *buf) {
  // Print at most MAX_PARSE_LEN characters to avoid spamming
  for (uint8_t i = 0; i < (uint8_t)MAX_PARSE_LEN; i++) {
    if (buf[i] == '\0') break;
    uart_write((uint8_t)buf[i]);
  }
  char ln[16];
  (void)sprintf(ln, " --- %u", (unsigned)strlen(buf));
  uart_write_str(ln);
  uart_write((uint8_t)'\n');
}
#endif

// ================== GRBL line parser (non-blocking) ==================

/**
 * @brief Incrementally parse characters from USART into a short line buffer.
 *
 * Collects characters until LF (\\n). CR (\\r) is ignored to support CR+LF sources.
 * Only the beginning of the line is stored (up to @ref MAX_PARSE_LEN - 1),
 * because matching is done on known message prefixes.
 *
 * @return Parsed @ref Status if a recognized message prefix is found
 *         at end-of-line; otherwise @ref UNKNOWN.
 */
static Status parse_status(void) {
  static char lineBuf[MAX_PARSE_LEN];
  static uint8_t idx = 0;

  while (uart_available()) {
    char c = (char)uart_read();
    if (c == '\r') {
      continue;  // skip CR
    }

    if (c == '\n') {  // end of line
      lineBuf[idx] = '\0';
      idx = 0;

      #ifdef DEBUG
      debugPrint(lineBuf);
      #endif

      if (lineBuf[0] == '\0') {
        return UNKNOWN;
      }

      if (strncmp(lineBuf, MSG_BOOTED, strlen(MSG_BOOTED)) == 0) return BOOTED;
      if (strncmp(lineBuf, MSG_IDLE,   strlen(MSG_IDLE))   == 0) return IDLE;
      if (strncmp(lineBuf, MSG_RUN,    strlen(MSG_RUN))    == 0) return RUN;
      if (strncmp(lineBuf, MSG_HOLD,   strlen(MSG_HOLD))   == 0) return HOLD;
      if (strncmp(lineBuf, MSG_JOG,    strlen(MSG_JOG))    == 0) return JOG;
      if (strncmp(lineBuf, MSG_DOOR,   strlen(MSG_DOOR))   == 0) return DOOR;
      if (strncmp(lineBuf, MSG_HOME,   strlen(MSG_HOME))   == 0) return HOME;
      if (strncmp(lineBuf, MSG_ALARM,  strlen(MSG_ALARM))  == 0) return ALARM;

      return UNKNOWN;  // complete line but not matched any message
    }

    // Store only the initial part needed for prefix matching
    if (idx < (sizeof(lineBuf) - 1u)) {
      lineBuf[idx++] = c;
    }
    // else: silently drop extra chars
  }

  return UNKNOWN;  // no full line yet
}

// ================== State ==================
static bool     seenBooted           = false;
static Status   lastShown            = UNKNOWN;
static uint32_t lastBlinkToggleMs    = 0;
static bool     blinkPhase           = false;  // false: red, true: purple
static uint32_t lastKnownStatusMs    = 0;
static uint32_t lastRequestMs        = 0;

// ================== Arduino lifecycle ==================

/**
 * @brief Arduino setup: init UART, LED, and start blinking until booted message arrives.
 */
void setup(void) {
  uart_init();

  // LED pin as output
  PORTA.DIR |= PIN3_bm;

  // NeoPixel init
  leds.begin();
  leds.setBrightness(BRIGHTNESS);

  // Startup: blink red/purple until BOOTED appears
  setColor(COL_RED);
  lastBlinkToggleMs = millis();
}

/**
 * @brief Main loop: parse status, request status periodically, and drive LED.
 */
void loop(void) {
  const uint32_t now = millis();

  // Parse any incoming line
  const Status st = parse_status();

  if (st != UNKNOWN) {
    if (st == BOOTED) {
      seenBooted        = true;    // connected and ready
      lastKnownStatusMs = now;
      lastRequestMs     = now;     // first "?\n" after REQUEST_TIMEOUT_MS
      if (st != lastShown) {
        showStatus(st);
        lastShown = st;
      }
    } else if (seenBooted) {
      lastKnownStatusMs = now;
      if (st != lastShown) {
        showStatus(st);
        lastShown = st;
      }
    }
    // else: not yet booted; keep blinking logic below
  }

  // If no new status for REQUEST_TIMEOUT_MS, ask GRBL for status with "?\n"
  if (((now - lastKnownStatusMs) >= REQUEST_TIMEOUT_MS) &&
      ((now - lastRequestMs)    >= REQUEST_TIMEOUT_MS)) {
    uart_write_str("?\n");
    lastRequestMs = now;
  }

  // BEFORE BOOTED: blink red <-> purple
  if (!seenBooted) {
    if ((now - lastBlinkToggleMs) >= BLINK_INTERVAL) {
      blinkPhase = !blinkPhase;
      setColor(blinkPhase ? COL_PUR : COL_RED);
      lastBlinkToggleMs = now;
    }
    return;  // wait for BOOTED
  }
}