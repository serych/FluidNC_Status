#include <Arduino.h>
#include <tinyNeoPixel_Static.h>

#define LED PIN_PA3
#define NUM_LEDS 2  // one onboard and one external, but you can add more to the chain
#define BRIGHTNESS 31 // total brightness of the LEDs

// Colors in RGB hexa format (feel free to change values or add more colors)
#define COL_RED 0xff0000  // red
#define COL_ORA 0xff5f00  // orange
#define COL_GRN 0x00ff00  // green
#define COL_CYA 0x007fff  // cyan
#define COL_PUR 0xff00ff  // purple

// UART speed
#define SPEED 115200 //Grbl UART interface speed 

// Timings (ms)
#define BLINK_INTERVAL 250        // startup blink until BOOTED seen
#define REQUEST_TIMEOUT_MS 1000   // if no status for this long, send "?\n"
#define STATUS_TIMEOUT_MS 5000    // if still no status for this long, show fallback

// GRBL message prefixes (feel free to add other, add the appropriate statuses below 
// and add lines to parse_status and show_status functions)
#define MSG_BOOTED "Grbl"
#define MSG_IDLE "<Idle"
#define MSG_RUN "<Run"
#define MSG_HOLD "<Hold"

// Status enumeration constants
enum Status {
  BOOTED,
  IDLE,
  RUN,
  HOLD,
  UNKNOWN = 255
};

// LED driver setup
byte pixels[NUM_LEDS * 3]; // data structure needed by the tinyNeoPixel library
tinyNeoPixel leds = tinyNeoPixel(NUM_LEDS, LED, NEO_GRB + NEO_KHZ800, pixels); // object inicialization

// -----------------------------------------------------------------------------
// Helper functions
 
// sets the color of the LED chain 
void setColor(uint32_t color) {
  leds.fill(color, 0, NUM_LEDS);
  leds.show();
}

// changes the color of LEDs according to status found
void showStatus(Status st) {
  switch (st) {
    case BOOTED: setColor(COL_RED); break; // briefly red after boot (or fallback)
    case IDLE:   setColor(COL_GRN); break;
    case RUN:    setColor(COL_CYA); break;
    case HOLD:   setColor(COL_ORA); break;
    // add more similar lines if you'll add more statuses
    default:     break;
  }
}

// -----------------------------------------------------------------------------
// GRBL status parser (non-blocking)
// tries to parse the line after each received character and returns UNKNOWN or status which was found 
Status parse_status() {
  static char lineBuf[64];
  static uint8_t idx = 0;

  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;

    if (c == '\n') { 
      lineBuf[idx] = '\0';
      idx = 0;

      if (lineBuf[0] == '\0') return UNKNOWN;

      if (strncmp(lineBuf, MSG_BOOTED, strlen(MSG_BOOTED)) == 0) return BOOTED;
      if (strncmp(lineBuf, MSG_IDLE,   strlen(MSG_IDLE))   == 0) return IDLE;
      if (strncmp(lineBuf, MSG_RUN,    strlen(MSG_RUN))    == 0) return RUN;
      if (strncmp(lineBuf, MSG_HOLD,   strlen(MSG_HOLD))   == 0) return HOLD;
      // add more similar lines if you'll add more statuses

      return UNKNOWN; // complete line but not matched
    }

    if (idx < sizeof(lineBuf) - 1) lineBuf[idx++] = c;
    else idx = 0; // overflow -> reset this line
  }

  return UNKNOWN; // no full line yet
}

// -----------------------------------------------------------------------------
// State
bool seenBooted = false;  // machine is still booting
Status lastShown = UNKNOWN;
uint32_t lastBlinkToggleMs = 0;  // for BLINK_INTERVAL
bool blinkPhase = false; // false: red, true: purple
uint32_t lastKnownStatusMs = 0;  // for STATUS_TIMEOUT_MS
uint32_t lastRequestMs = 0;   // for REQUEST_TIMEOUT_MS

// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(SPEED);
  pinMode(LED, OUTPUT);
  leds.begin();
  leds.setBrightness(BRIGHTNESS);

  // Startup: blink red/purple until BOOTED appears
  setColor(COL_RED);
  lastBlinkToggleMs = millis();
}

// -----------------------------------------------------------------------------
void loop() {
  const uint32_t now = millis();  // timestamp of this loop iteration

  // Parse any incoming line
  Status st = parse_status();

  if (st != UNKNOWN) {
    if (st == BOOTED) {
      seenBooted = true;
      // Start the request cadence *after* booted is seen
      lastKnownStatusMs = now;
      lastRequestMs = now; // first "?" will go out after REQUEST_TIMEOUT_MS
    } else if (seenBooted) {
      lastKnownStatusMs = now; // any known status refreshes the timer
    }

    if (seenBooted && st != lastShown) {
      showStatus(st);
      lastShown = st;
    }
  }

  // BEFORE BOOTED: blink red <-> purple
  if (!seenBooted) {
    if ((now - lastBlinkToggleMs) >= BLINK_INTERVAL) {
      blinkPhase = !blinkPhase;
      setColor(blinkPhase ? COL_PUR : COL_RED);
      lastBlinkToggleMs = now;
    }
    return; // wait for BOOTED
  }

  // AFTER BOOTED:
  // 1) If no new status for REQUEST_TIMEOUT_MS, ask GRBL for status with "?\n"
  if ((now - lastKnownStatusMs) >= REQUEST_TIMEOUT_MS &&
      (now - lastRequestMs)    >= REQUEST_TIMEOUT_MS) {
    Serial.write('?');
    Serial.write('\n');
    lastRequestMs = now;
  }

  // 2) If STILL no status for STATUS_TIMEOUT_MS, show fallback (solid red)
  if ((now - lastKnownStatusMs) > STATUS_TIMEOUT_MS) {
    if (lastShown != BOOTED) { // reuse BOOTED color for fallback
      setColor(COL_RED);
      lastShown = BOOTED;
    }
  }

  // Otherwise: keep showing the last known status color
}
