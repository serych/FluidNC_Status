#define F_CPU 20000000UL
#ifndef USART0_BAUD_RATE
#define USART0_BAUD_RATE(BAUD_RATE) ((float)(F_CPU * 64 / (16 * (float)BAUD_RATE)) + 0.5)
#endif
// #define DEBUG 1
#include <tinyNeoPixel_Static.h>

// ================== Hardware / Pins ==================
#define LED PIN_PA3
#define TX PIN_PA1  // just for info
#define RX PIN_PA2  // just for info
#define BAUDRATE 115200
#define NUM_LEDS 1
#define BRIGHTNESS 31

// ================== Colors ==================
#define COL_RED 0xff0000
#define COL_ORA 0xff5f00
#define COL_YEL 0xffcf00
#define COL_GRN 0x00ff00
#define COL_CYA 0x007fff
#define COL_PUR 0xff00ff   // purple

// ================== Timings (ms) ==================
#define BLINK_INTERVAL      250     // startup blink until BOOTED seen
#define REQUEST_TIMEOUT_MS  5000    // if no status for this long, send "?\n"
#define STATUS_TIMEOUT_MS   5000    // if still no status, show fallback

// ================== GRBL message prefixes ==================
#define MSG_BOOTED "[MSG:INFO: Connected"
#define MSG_IDLE   "<Idle"
#define MSG_RUN    "<Run"
#define MSG_HOLD   "<Hold"
#define MSG_JOG    "<Jog"
#define MSG_DOOR   "<Door"
#define MSG_HOME   "<Home"
#define MSG_ALARM   "<Alarm"



enum Status {
  BOOTED,
  IDLE,
  RUN,
  HOLD,
  JOG,
  DOOR,
  HOME,
  ALARM,
  UNKNOWN = 255
};

// ================== NeoPixel ==================
byte pixels[NUM_LEDS * 3];
tinyNeoPixel leds = tinyNeoPixel(NUM_LEDS, LED, NEO_GRB + NEO_KHZ800, pixels);

// ================== USART0 (register-level) ==================
void USART0_init()
{
  // Route USART0 to ALTERNATE pins (bit 0 = 1)
 
  // Set directions for the *alternate* pins
    PORTMUX.CTRLB |= 1; // |= 1 Alternate, &= 0xfe Default USART pins
    PORTA.DIR &= ~PIN2_bm; //input (RX)
    PORTA.DIR |= PIN1_bm; //output (TX) 
    PORTA.DIR |= PIN3_bm; //output (LED pin) 

  // UART 8N1, async
  //USART0.CTRLC = USART_CHSIZE_8BIT_gc | USART_SBMODE_1BIT_gc | USART_PMODE_DISABLED_gc;

  // Baud
  USART0.BAUD = (uint16_t)USART0_BAUD_RATE(BAUDRATE);

  // Enable RX & TX
  USART0.CTRLB |= USART_RXEN_bm | USART_TXEN_bm;
}

// Non-blocking: true if a byte is waiting
bool uart_available() { return (USART0.STATUS & USART_RXCIF_bm); }
// Read one byte (call only if available)
uint8_t uart_read()   { return USART0.RXDATAL; }
// Write one byte
void uart_write(uint8_t b) {
  while (!(USART0.STATUS & USART_DREIF_bm)) { /* wait */ }
  USART0.TXDATAL = b;
}
//void uart_write_str(const char *s) { while (*s) uart_write((uint8_t)*s++); }
void uart_write_str(const char *str)
{
    for(size_t i = 0; i < strlen(str); i++)
    {
        uart_write(str[i]);
    }
}

// ================== LED helpers ==================
void setColor(uint32_t color) { leds.fill(color, 0, NUM_LEDS); leds.show(); }
void showStatus(Status st) {
  switch (st) {
    case BOOTED: setColor(COL_GRN); break; // also used as fallback
    case IDLE:   setColor(COL_GRN); break;
    case RUN:    setColor(COL_CYA); break;
    case HOLD:   setColor(COL_YEL); break;
    case JOG:    setColor(COL_PUR); break; 
    case DOOR:   setColor(COL_ORA); break;
    case HOME:   setColor(COL_PUR); break;
    case ALARM:  setColor(COL_RED); break; 
    default:     break;
  }
}

//==================== Debug print ==================
void debugPrint(const char *buf){
        for (uint8_t i = 0; i < 25; i++)
      {
        uart_write(buf[i]);
      }
      uart_write(' ');
      char ln[10]; 
      sprintf(ln, " --- %d", strlen(buf));
      uart_write_str(ln);
      uart_write('\n');
}

// ================== GRBL line parser (non-blocking) ==================
Status parse_status() {
  static char lineBuf[40];
  static uint8_t idx = 0;

  while (uart_available()) {
    char c = (char)uart_read();
    //uart_write(c);
    if (c == '\r') continue;

    if (c == '\n') {
      lineBuf[idx] = '\0';
      idx = 0;
      #ifdef DEBUG
      debugPrint(lineBuf);
      #endif
      if (lineBuf[0] == '\0') return UNKNOWN;

      if (strncmp(lineBuf, MSG_BOOTED, strlen(MSG_BOOTED)) == 0) return BOOTED;
      if (strncmp(lineBuf, MSG_IDLE,   strlen(MSG_IDLE))   == 0) return IDLE;
      if (strncmp(lineBuf, MSG_RUN,    strlen(MSG_RUN))    == 0) return RUN;
      if (strncmp(lineBuf, MSG_HOLD,   strlen(MSG_HOLD))   == 0) return HOLD;
      if (strncmp(lineBuf, MSG_JOG,    strlen(MSG_JOG))   == 0) return JOG;
      if (strncmp(lineBuf, MSG_DOOR,   strlen(MSG_DOOR))   == 0) return DOOR;
      if (strncmp(lineBuf, MSG_HOME,   strlen(MSG_HOME))   == 0) return HOME;
      if (strncmp(lineBuf, MSG_ALARM,  strlen(MSG_ALARM))   == 0) return ALARM;

      return UNKNOWN; // complete line but not matched
    }

    if (idx < sizeof(lineBuf) - 1) lineBuf[idx++] = c;
    //else idx = 0; // overflow -> reset this line
  }

  return UNKNOWN; // no full line yet
}

// ================== State ==================
bool seenBooted = false;
Status lastShown = UNKNOWN;
uint32_t lastBlinkToggleMs = 0;
bool blinkPhase = false; // false: red, true: purple
uint32_t lastKnownStatusMs = 0;
uint32_t lastRequestMs = 0;

// ================== Arduino lifecycle ==================
void setup() {
  USART0_init();

  pinMode(LED, OUTPUT);
  leds.begin();
  leds.setBrightness(BRIGHTNESS);

  // Startup: blink red/purple until BOOTED appears
  setColor(COL_RED);
  lastBlinkToggleMs = millis();
}

void loop() {
  const uint32_t now = millis();

  // Parse any incoming line
  Status st = parse_status();

  if (st != UNKNOWN) {
    if (st == BOOTED) {
      seenBooted = true;
      lastKnownStatusMs = now;
      lastRequestMs = now; // first "?" after REQUEST_TIMEOUT_MS
      if (st != lastShown) { showStatus(st); lastShown = st; }
    } else if (seenBooted) { // ODSTRANIT !
      lastKnownStatusMs = now;
      if (st != lastShown) { showStatus(st); lastShown = st; }
    }
  }

  // BEFORE BOOTED: blink red <-> purple
    // 1) If no new status for REQUEST_TIMEOUT_MS, ask GRBL for status with "?\n"
  if ((now - lastKnownStatusMs) >= REQUEST_TIMEOUT_MS &&
      (now - lastRequestMs)    >= REQUEST_TIMEOUT_MS) {
    uart_write_str("?\n");
    lastRequestMs = now;
  }

  if (!seenBooted) {
    if ((now - lastBlinkToggleMs) >= BLINK_INTERVAL) {
      blinkPhase = !blinkPhase;
      setColor(blinkPhase ? COL_PUR : COL_RED);
      lastBlinkToggleMs = now;
      // uart_write_str("?\n");
    }
    return; // wait for BOOTED
  }

  // AFTER BOOTED:


  // 2) If STILL no status for STATUS_TIMEOUT_MS, show fallback (solid red)
  /*
  if ((now - lastKnownStatusMs) > STATUS_TIMEOUT_MS) {
    if (lastShown != BOOTED) {
      setColor(COL_RED);
      lastShown = BOOTED;
    }
  }
  */  
}
