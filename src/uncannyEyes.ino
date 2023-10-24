//--------------------------------------------------------------------------
// Uncanny eyes for Adafruit 1.5" OLED (product #1431) or 1.44" TFT LCD
// (#2088).  Works on PJRC Teensy 3.x and on Adafruit M0 and M4 boards
// (Feather, Metro, etc.).  This code uses features specific to these
// boards and WILL NOT work on normal Arduino or other boards!
//
// SEE FILE "config.h" FOR MOST CONFIGURATION (graphics, pins, display type,
// etc).  Probably won't need to edit THIS file unless you're doing some
// extremely custom modifications.
//
// Adafruit invests time and resources providing this open source code,
// please support Adafruit and open-source hardware by purchasing products
// from Adafruit!
//
// Written by Phil Burgess / Paint Your Dragon for Adafruit Industries.
// MIT license.  SPI FIFO insight from Paul Stoffregen's ILI9341_t3 library.
// Inspired by David Boccabella's (Marcwolf) hybrid servo/OLED eye concept.
//--------------------------------------------------------------------------

#include <Arduino_GFX_Library.h>

typedef struct {        // Struct is defined before including config.h --
  int8_t  select;       // pin numbers for each eye's screen select line
  int8_t  wink;         // and wink button (or -1 if none) specified there,
  uint8_t rotation;     // also display rotation.
} eyeInfo_t;

#include "config.h"     // ****** CONFIGURATION IS DONE IN HERE ******

extern void user_setup(void); // Functions in the user*.cpp files
extern void user_loop(void);

#ifdef PIXEL_DOUBLE
  // For the 240x240 TFT, pixels are rendered in 2x2 blocks for an
  // effective resolution of 120x120. M0 boards just don't have the
  // space or speed to handle an eye at the full resolution of this
  // display (and for M4 boards, take a look at the M4_Eyes project
  // instead). 120x120 doesn't quite match the resolution of the
  // TFT & OLED this project was originally developed for. Rather
  // than make an entirely new alternate set of graphics for every
  // eye (would be a huge undertaking), this currently just crops
  // four pixels all around the perimeter.
  #define SCREEN_X_START 4
  #define SCREEN_X_END   (SCREEN_WIDTH - 4)
  #define SCREEN_Y_START 4
  #define SCREEN_Y_END   (SCREEN_HEIGHT - 4)
#else
  #define SCREEN_X_START 0
  #define SCREEN_X_END   SCREEN_WIDTH
  #define SCREEN_Y_START 0
  #define SCREEN_Y_END   SCREEN_HEIGHT
#endif

// A simple state machine is used to control eye blinks/winks:
#define NOBLINK 0       // Not currently engaged in a blink
#define ENBLINK 1       // Eyelid is currently closing
#define DEBLINK 2       // Eyelid is currently opening
typedef struct {
  uint8_t  state;       // NOBLINK/ENBLINK/DEBLINK
  uint32_t duration;    // Duration of blink state (micros)
  uint32_t startTime;   // Time (micros) of last state change
} eyeBlink;

#define NUM_EYES (sizeof eyeInfo / sizeof eyeInfo[0]) // config.h pin list

struct {                // One-per-eye structure
  Arduino_DataBus *bus;  // bus for display
  Arduino_GFX *display; // -> OLED/TFT object
  eyeBlink     blink;   // Current blink/wink state
} eye[NUM_EYES];

uint8_t           dmaIdx = 0; // Active DMA buffer # (alternate fill/send)
#ifdef PIXEL_DOUBLE
 uint32_t          dmaBuf[2][SCREEN_WIDTH/2]; // Two 240-pixel buffers (32bit for doubling)
#else
 uint16_t          dmaBuf[2][SCREEN_WIDTH]; // Two 128-pixel buffers
#endif

uint32_t startTime;  // For FPS indicator

#if defined(SYNCPIN) && (SYNCPIN >= 0)
#include <Wire.h>
// If two boards are synchronized over I2C, this struct is passed from one
// to other. No device-independent packing & unpacking is performed...both
// boards are expected to be the same architecture & endianism.
struct {
  uint16_t iScale;  // These are basically the same arguments as
  uint16_t scleraX; // drawEye() expects, explained in that function.
  uint16_t scleraY;
  uint8_t  uT;
  uint8_t  lT;
} syncStruct = { 512,
  (SCLERA_WIDTH-SCREEN_WIDTH)/2, (SCLERA_HEIGHT-SCREEN_HEIGHT)/2, 0, 0 };

void wireCallback(int n) {
  if(n == sizeof syncStruct) {
    // Read 'n' bytes from I2C into syncStruct
    uint8_t *ptr = (uint8_t *)&syncStruct;
    for(uint8_t i=0; i < sizeof syncStruct; i++) {
      ptr[i] = Wire.read();
    }
  }
}

bool receiver = false;
#endif // SYNCPIN

// INITIALIZATION -- runs once at startup ----------------------------------

void setup(void) {
  uint8_t e; // Eye index, 0 to NUM_EYES-1

#if defined(SYNCPIN) && (SYNCPIN >= 0) // If using I2C sync...
  pinMode(SYNCPIN, INPUT_PULLUP);      // Check for jumper to ground
  if(!digitalRead(SYNCPIN)) {          // If there...
    receiver = true;                   // Set this one up as receiver
    Wire.begin(SYNCADDR);
    Wire.onReceive(wireCallback);
  } else {
    Wire.begin();                      // Else set up as sender
  }
#endif

  Serial.begin(115200);
  //while (!Serial);
  Serial.println("Init");
  randomSeed(analogRead(A3)); // Seed random() from floating analog input

#ifdef DISPLAY_BACKLIGHT
  // Enable backlight pin, initially off
  Serial.println("Backlight off");
  pinMode(DISPLAY_BACKLIGHT, OUTPUT);
  digitalWrite(DISPLAY_BACKLIGHT, LOW);
#endif

  user_setup();

  // Initialize eye objects based on eyeInfo list in config.h:
  for(e=0; e<NUM_EYES; e++) {
    Serial.print("Create display #"); Serial.println(e);

    eye[e].bus = new Arduino_ESP32SPI(2 /* DC */, eyeInfo[e].select /* CS */, 6 /* SCK */, 7 /* MOSI */, GFX_NOT_DEFINED /* MISO */);
    eye[e].display     = new Arduino_GC9A01 (eye[e].bus, GFX_NOT_DEFINED /* RST */, eyeInfo[e].rotation /* rotation */, true /* IPS */);
    eye[e].blink.state = NOBLINK;
    // If project involves only ONE eye and NO other SPI devices, its
    // select line can be permanently tied to GND and corresponding pin
    // in config.h set to -1.  Best to use it though.
    if(eyeInfo[e].select >= 0) {
      pinMode(eyeInfo[e].select, OUTPUT);
      digitalWrite(eyeInfo[e].select, HIGH); // Deselect them all
    }
    // Also set up an individual eye-wink pin if defined:
    if(eyeInfo[e].wink >= 0) pinMode(eyeInfo[e].wink, INPUT_PULLUP);
  }
#if defined(BLINK_PIN) && (BLINK_PIN >= 0)
  pinMode(BLINK_PIN, INPUT_PULLUP); // Ditto for all-eyes blink pin
#endif

#if defined(DISPLAY_RESET) && (DISPLAY_RESET >= 0)
  // Because both displays share a common reset pin, -1 is passed to
  // the display constructor above to prevent the begin() function from
  // resetting both displays after one is initialized.  Instead, handle
  // the reset manually here to take care of both displays just once:
  Serial.println("Reset displays");
  pinMode(DISPLAY_RESET, OUTPUT);
  digitalWrite(DISPLAY_RESET, LOW);  delay(1);
  digitalWrite(DISPLAY_RESET, HIGH); delay(50);
  // Alternately, all display reset pin(s) could be connected to the
  // microcontroller reset, in which case DISPLAY_RESET should be set
  // to -1 or left undefined in config.h.
#else
  // If no DISPLAY_RESET pin is defined, the board might have an auto-
  // reset circuit for the display (e.g. TFT Gizmo). Pause for just a
  // tiny moment to allow it to work, otherwise commands will be issued
  // too soon and the display won't correctly initialize.
  delay(75);
#endif

  // After all-displays reset, now call init/begin func for each display:
  for(e=0; e<NUM_EYES; e++) {
    eye[e].display->begin();
    Serial.println("Rotate");
    eye[e].display->setRotation(eyeInfo[e].rotation);
  }
  Serial.println("done");

#if defined(LOGO_TOP_WIDTH) || defined(COLOR_LOGO_WIDTH)
  // I noticed lots of folks getting right/left eyes flipped, or
  // installing upside-down, etc.  Logo split across screens may help:
  int x = eye[0].display->width() * NUM_EYES / 2;
  int y = (eye[0].display->height() - LOGO_TOP_HEIGHT - LOGO_BOTTOM_HEIGHT) / 2;
  for(e=0; e<NUM_EYES; e++) { // Another pass, after all screen inits
    eye[e].display->fillScreen(0);
    #ifdef LOGO_TOP_WIDTH
      // Monochrome Adafruit logo is 2 mono bitmaps:
      eye[e].display->drawBitmap(x - LOGO_TOP_WIDTH / 2 - 20,
        y, logo_top, LOGO_TOP_WIDTH, LOGO_TOP_HEIGHT, 0xFFFF);
      eye[e].display->drawBitmap(x - LOGO_BOTTOM_WIDTH/2,
        y + LOGO_TOP_HEIGHT, logo_bottom, LOGO_BOTTOM_WIDTH, LOGO_BOTTOM_HEIGHT,
        0xFFFF);
    #else
      // Color sponsor logo is one RGB bitmap:
      eye[e].display->fillScreen(color_logo[0]);
      eye[e].display->drawRGBBitmap(
        (eye[e].display->width()  - COLOR_LOGO_WIDTH ) / 2,
        (eye[e].display->height() - COLOR_LOGO_HEIGHT) / 2,
        color_logo, COLOR_LOGO_WIDTH, COLOR_LOGO_HEIGHT);
    #endif
    x -= eye[e].display->width();
  }

  // After logo is drawn
  #ifdef DISPLAY_BACKLIGHT
    int i;
    Serial.println("Fade in backlight");
    analogWriteResolution(8);
    for(i=0; i<=BACKLIGHT_MAX; i++) { // Fade logo in
      analogWrite(DISPLAY_BACKLIGHT, i);
      delay(2);
    }
    delay(1400); // Pause for screen layout/orientation
    Serial.println("Fade out backlight");
    for(; i>=0; i--) {
      analogWrite(DISPLAY_BACKLIGHT, i);
      delay(2);
    }
    for(e=0; e<NUM_EYES; e++) { // Clear display(s)
      eye[e].display->fillScreen(0);
    }
    delay(100);
  #else
    delay(2000); // Pause for screen layout/orientation
  #endif // DISPLAY_BACKLIGHT
#endif // LOGO_TOP_WIDTH

#ifdef DISPLAY_BACKLIGHT
  Serial.println("Backlight on!");
  analogWrite(DISPLAY_BACKLIGHT, BACKLIGHT_MAX);
#endif

  startTime = millis(); // For frame-rate calculation
}


// EYE-RENDERING FUNCTION --------------------------------------------------


void drawEye( // Renders one eye.  Inputs must be pre-clipped & valid.
  uint8_t  e,       // Eye array index; 0 or 1 for left/right
  uint16_t iScale,  // Scale factor for iris (0-1023)
  uint16_t scleraX, // First pixel X offset into sclera image
  uint16_t scleraY, // First pixel Y offset into sclera image
  uint8_t  uT,      // Upper eyelid threshold value
  uint8_t  lT) {    // Lower eyelid threshold value

  uint8_t  screenX, screenY, scleraXsave;
  int16_t  irisX, irisY;
  uint16_t p, a;
  uint32_t d;

#if defined(SYNCPIN) && (SYNCPIN >= 0)
  if(receiver) {
    // Overwrite arguments with values in syncStruct.  Disable interrupts
    // briefly so new data can't overwrite the struct in mid-parse.
    noInterrupts();
    iScale  = syncStruct.iScale;
    // Screen is mirrored, this 'de-mirrors' the eye X direction
    scleraX = SCLERA_WIDTH - 1 - SCREEN_WIDTH - syncStruct.scleraX;
    scleraY = syncStruct.scleraY;
    uT      = syncStruct.uT;
    lT      = syncStruct.lT;
    interrupts();
  } else {
    // Stuff arguments into syncStruct and send to receiver
    syncStruct.iScale  = iScale;
    syncStruct.scleraX = scleraX;
    syncStruct.scleraY = scleraY;
    syncStruct.uT      = uT;
    syncStruct.lT      = lT;
    Wire.beginTransmission(SYNCADDR);
    Wire.write((char *)&syncStruct, sizeof syncStruct);
    Wire.endTransmission();
  }
#endif

#ifdef PIXEL_DOUBLE
  scleraX += 4;
  scleraY += 4;
#endif

  uint8_t  irisThreshold = (128 * (1023 - iScale) + 512) / 1024;
  uint32_t irisScale     = IRIS_MAP_HEIGHT * 65536 / irisThreshold;

  // Now just issue raw 16-bit values for every pixel...
  scleraXsave = scleraX + SCREEN_X_START; // Save initial X value to reset on each line
  irisY       = scleraY - (SCLERA_HEIGHT - IRIS_HEIGHT) / 2;
  for(screenY=SCREEN_Y_START; screenY<SCREEN_Y_END; screenY++, scleraY++, irisY++) {
 #ifdef PIXEL_DOUBLE
    uint32_t *ptr = &dmaBuf[dmaIdx][0];
 #else
    uint16_t *ptr = &dmaBuf[dmaIdx][0];
 #endif
    scleraX = scleraXsave;
    irisX   = scleraXsave - (SCLERA_WIDTH - IRIS_WIDTH) / 2;
    for(screenX=SCREEN_X_START; screenX<SCREEN_X_END; screenX++, scleraX++, irisX++) {
      if((lower[screenY][screenX] <= lT) ||
         (upper[screenY][screenX] <= uT)) {             // Covered by eyelid
        p = 0;
      } else if((irisY < 0) || (irisY >= IRIS_HEIGHT) ||
                (irisX < 0) || (irisX >= IRIS_WIDTH)) { // In sclera
        p = sclera[scleraY][scleraX];
      } else {                                          // Maybe iris...
        p = polar[irisY][irisX];                        // Polar angle/dist
        d = p & 0x7F;                                   // Distance from edge (0-127)
        if(d < irisThreshold) {                         // Within scaled iris area
          d = d * irisScale / 65536;                    // d scaled to iris image height
          a = (IRIS_MAP_WIDTH * (p >> 7)) / 512;        // Angle (X)
          p = iris[d][a];                               // Pixel = iris
        } else {                                        // Not in iris
          p = sclera[scleraY][scleraX];                 // Pixel = sclera
        }
      }
 #ifdef PIXEL_DOUBLE
      // Swap bytes, duplicate low 16 to high 16 bits, store in DMA buf
      *ptr++ = p * 0x00010001;
 #else
      *ptr++ = p; // DMA: store in scanline buffer
 #endif
    } // end column

#ifdef PIXEL_DOUBLE
    eye[e].display->draw16bitRGBBitmap(0, (screenY - SCREEN_Y_START)*2,   (uint16_t *) &dmaBuf[dmaIdx][0], (SCREEN_X_END - SCREEN_X_START)*2, 1);
    eye[e].display->draw16bitRGBBitmap(0, (screenY - SCREEN_Y_START)*2+1, (uint16_t *) &dmaBuf[dmaIdx][0], (SCREEN_X_END - SCREEN_X_START)*2, 1);
#else
    eye[e].display->draw16bitRGBBitmap(0, (screenY - SCREEN_Y_START), (uint16_t *) &dmaBuf[dmaIdx][0], (SCREEN_X_END - SCREEN_X_START), 1);
#endif

  } // end scanline
}

// EYE ANIMATION -----------------------------------------------------------

const uint8_t ease[] = { // Ease in/out curve for eye movements 3*t^2-2*t^3
    0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  2,  2,  2,  3,   // T
    3,  3,  4,  4,  4,  5,  5,  6,  6,  7,  7,  8,  9,  9, 10, 10,   // h
   11, 12, 12, 13, 14, 15, 15, 16, 17, 18, 18, 19, 20, 21, 22, 23,   // x
   24, 25, 26, 27, 27, 28, 29, 30, 31, 33, 34, 35, 36, 37, 38, 39,   // 2
   40, 41, 42, 44, 45, 46, 47, 48, 50, 51, 52, 53, 54, 56, 57, 58,   // A
   60, 61, 62, 63, 65, 66, 67, 69, 70, 72, 73, 74, 76, 77, 78, 80,   // l
   81, 83, 84, 85, 87, 88, 90, 91, 93, 94, 96, 97, 98,100,101,103,   // e
  104,106,107,109,110,112,113,115,116,118,119,121,122,124,125,127,   // c
  128,130,131,133,134,136,137,139,140,142,143,145,146,148,149,151,   // J
  152,154,155,157,158,159,161,162,164,165,167,168,170,171,172,174,   // a
  175,177,178,179,181,182,183,185,186,188,189,190,192,193,194,195,   // c
  197,198,199,201,202,203,204,205,207,208,209,210,211,213,214,215,   // o
  216,217,218,219,220,221,222,224,225,226,227,228,228,229,230,231,   // b
  232,233,234,235,236,237,237,238,239,240,240,241,242,243,243,244,   // s
  245,245,246,246,247,248,248,249,249,250,250,251,251,251,252,252,   // o
  252,253,253,253,254,254,254,254,254,255,255,255,255,255,255,255 }; // n

#ifdef AUTOBLINK
uint32_t timeOfLastBlink = 0L, timeToNextBlink = 0L;
#endif

void frame( // Process motion for a single frame of left or right eye
  uint16_t        iScale) {     // Iris scale (0-1023) passed in
  static uint32_t frames   = 0; // Used in frame rate calculation
  static uint8_t  eyeIndex = 0; // eye[] array counter
  int16_t         eyeX, eyeY;
  uint32_t        t = micros(); // Time at start of function

  if(!(++frames & 255)) { // Every 256 frames...
    uint32_t elapsed = (millis() - startTime) / 1000;
    if(elapsed) Serial.println(frames / elapsed); // Print FPS
  }

  if(++eyeIndex >= NUM_EYES) eyeIndex = 0; // Cycle through eyes, 1 per call

  // X/Y movement

#if defined(JOYSTICK_X_PIN) && (JOYSTICK_X_PIN >= 0) && \
    defined(JOYSTICK_Y_PIN) && (JOYSTICK_Y_PIN >= 0)

  // Read X/Y from joystick, constrain to circle
  int16_t dx, dy;
  int32_t d;
  eyeX = analogRead(JOYSTICK_X_PIN); // Raw (unclipped) X/Y reading
  eyeY = analogRead(JOYSTICK_Y_PIN);
#ifdef JOYSTICK_X_FLIP
  eyeX = 1023 - eyeX;
#endif
#ifdef JOYSTICK_Y_FLIP
  eyeY = 1023 - eyeY;
#endif
  dx = (eyeX * 2) - 1023; // A/D exact center is at 511.5.  Scale coords
  dy = (eyeY * 2) - 1023; // X2 so range is -1023 to +1023 w/center at 0.
  if((d = (dx * dx + dy * dy)) > (1023 * 1023)) { // Outside circle
    d    = (int32_t)sqrt((float)d);               // Distance from center
    eyeX = ((dx * 1023 / d) + 1023) / 2;          // Clip to circle edge,
    eyeY = ((dy * 1023 / d) + 1023) / 2;          // scale back to 0-1023
  }

#else // Autonomous X/Y eye motion
      // Periodically initiates motion to a new random point, random speed,
      // holds there for random period until next motion.

  static boolean  eyeInMotion      = false;
  static int16_t  eyeOldX=512, eyeOldY=512, eyeNewX=512, eyeNewY=512;
  static uint32_t eyeMoveStartTime = 0L;
  static int32_t  eyeMoveDuration  = 0L;

  int32_t dt = t - eyeMoveStartTime;      // uS elapsed since last eye event
  if(eyeInMotion) {                       // Currently moving?
    if(dt >= eyeMoveDuration) {           // Time up?  Destination reached.
      eyeInMotion      = false;           // Stop moving
      eyeMoveDuration  = random(3000000); // 0-3 sec stop
      eyeMoveStartTime = t;               // Save initial time of stop
      eyeX = eyeOldX = eyeNewX;           // Save position
      eyeY = eyeOldY = eyeNewY;
    } else { // Move time's not yet fully elapsed -- interpolate position
      int16_t e = ease[255 * dt / eyeMoveDuration] + 1;   // Ease curve
      eyeX = eyeOldX + (((eyeNewX - eyeOldX) * e) / 256); // Interp X
      eyeY = eyeOldY + (((eyeNewY - eyeOldY) * e) / 256); // and Y
    }
  } else {                                // Eye stopped
    eyeX = eyeOldX;
    eyeY = eyeOldY;
    if(dt > eyeMoveDuration) {            // Time up?  Begin new move.
      int16_t  dx, dy;
      uint32_t d;
      do {                                // Pick new dest in circle
        eyeNewX = random(1024);
        eyeNewY = random(1024);
        dx      = (eyeNewX * 2) - 1023;
        dy      = (eyeNewY * 2) - 1023;
      } while((d = (dx * dx + dy * dy)) > (1023 * 1023)); // Keep trying
      eyeMoveDuration  = random(72000, 144000); // ~1/14 - ~1/7 sec
      eyeMoveStartTime = t;               // Save initial time of move
      eyeInMotion      = true;            // Start move on next frame
    }
  }

#endif // JOYSTICK_X_PIN etc.

  // Blinking

#ifdef AUTOBLINK
  // Similar to the autonomous eye movement above -- blink start times
  // and durations are random (within ranges).
  if((t - timeOfLastBlink) >= timeToNextBlink) { // Start new blink?
    timeOfLastBlink = t;
    uint32_t blinkDuration = random(36000, 72000); // ~1/28 - ~1/14 sec
    // Set up durations for both eyes (if not already winking)
    for(uint8_t e=0; e<NUM_EYES; e++) {
      if(eye[e].blink.state == NOBLINK) {
        eye[e].blink.state     = ENBLINK;
        eye[e].blink.startTime = t;
        eye[e].blink.duration  = blinkDuration;
      }
    }
    timeToNextBlink = blinkDuration * 3 + random(4000000);
  }
#endif

  if(eye[eyeIndex].blink.state) { // Eye currently blinking?
    // Check if current blink state time has elapsed
    if((t - eye[eyeIndex].blink.startTime) >= eye[eyeIndex].blink.duration) {
      // Yes -- increment blink state, unless...
      if((eye[eyeIndex].blink.state == ENBLINK) && ( // Enblinking and...
#if defined(BLINK_PIN) && (BLINK_PIN >= 0)
        (digitalRead(BLINK_PIN) == LOW) ||           // blink or wink held...
#endif
        ((eyeInfo[eyeIndex].wink >= 0) &&
         digitalRead(eyeInfo[eyeIndex].wink) == LOW) )) {
        // Don't advance state yet -- eye is held closed instead
      } else { // No buttons, or other state...
        if(++eye[eyeIndex].blink.state > DEBLINK) { // Deblinking finished?
          eye[eyeIndex].blink.state = NOBLINK;      // No longer blinking
        } else { // Advancing from ENBLINK to DEBLINK mode
          eye[eyeIndex].blink.duration *= 2; // DEBLINK is 1/2 ENBLINK speed
          eye[eyeIndex].blink.startTime = t;
        }
      }
    }
  } else { // Not currently blinking...check buttons!
#if defined(BLINK_PIN) && (BLINK_PIN >= 0)
    if(digitalRead(BLINK_PIN) == LOW) {
      // Manually-initiated blinks have random durations like auto-blink
      uint32_t blinkDuration = random(36000, 72000);
      for(uint8_t e=0; e<NUM_EYES; e++) {
        if(eye[e].blink.state == NOBLINK) {
          eye[e].blink.state     = ENBLINK;
          eye[e].blink.startTime = t;
          eye[e].blink.duration  = blinkDuration;
        }
      }
    } else
#endif
    if((eyeInfo[eyeIndex].wink >= 0) &&
       (digitalRead(eyeInfo[eyeIndex].wink) == LOW)) { // Wink!
      eye[eyeIndex].blink.state     = ENBLINK;
      eye[eyeIndex].blink.startTime = t;
      eye[eyeIndex].blink.duration  = random(45000, 90000);
    }
  }

  // Process motion, blinking and iris scale into renderable values

  // Scale eye X/Y positions (0-1023) to pixel units used by drawEye()
  eyeX = map(eyeX, 0, 1023, 0, SCLERA_WIDTH  - SCREEN_WIDTH);
  eyeY = map(eyeY, 0, 1023, 0, SCLERA_HEIGHT - SCREEN_HEIGHT);
  if(eyeIndex == 1) eyeX = (SCLERA_WIDTH - SCREEN_WIDTH) - eyeX; // Mirrored display

  // Horizontal position is offset so that eyes are very slightly crossed
  // to appear fixated (converged) at a conversational distance.  Number
  // here was extracted from my posterior and not mathematically based.
  // I suppose one could get all clever with a range sensor, but for now...
  if(NUM_EYES > 1) eyeX += 4;
  if(eyeX > (SCLERA_WIDTH - SCREEN_WIDTH)) eyeX = (SCLERA_WIDTH - SCREEN_WIDTH);

  // Eyelids are rendered using a brightness threshold image.  This same
  // map can be used to simplify another problem: making the upper eyelid
  // track the pupil (eyes tend to open only as much as needed -- e.g. look
  // down and the upper eyelid drops).  Just sample a point in the upper
  // lid map slightly above the pupil to determine the rendering threshold.
  static uint8_t uThreshold = 128;
  uint8_t        lThreshold, n;
#ifdef TRACKING
  int16_t sampleX = SCLERA_WIDTH  / 2 - (eyeX / 2), // Reduce X influence
          sampleY = SCLERA_HEIGHT / 2 - (eyeY + IRIS_HEIGHT / 4);
  // Eyelid is slightly asymmetrical, so two readings are taken, averaged
  if(sampleY < 0) n = 0;
  else            n = (upper[sampleY][sampleX] +
                       upper[sampleY][SCREEN_WIDTH - 1 - sampleX]) / 2;
  uThreshold = (uThreshold * 3 + n) / 4; // Filter/soften motion
  // Lower eyelid doesn't track the same way, but seems to be pulled upward
  // by tension from the upper lid.
  lThreshold = 254 - uThreshold;
#else // No tracking -- eyelids full open unless blink modifies them
  uThreshold = lThreshold = 0;
#endif

  // The upper/lower thresholds are then scaled relative to the current
  // blink position so that blinks work together with pupil tracking.
  if(eye[eyeIndex].blink.state) { // Eye currently blinking?
    uint32_t s = (t - eye[eyeIndex].blink.startTime);
    if(s >= eye[eyeIndex].blink.duration) s = 255;   // At or past blink end
    else s = 255 * s / eye[eyeIndex].blink.duration; // Mid-blink
    s          = (eye[eyeIndex].blink.state == DEBLINK) ? 1 + s : 256 - s;
    n          = (uThreshold * s + 254 * (257 - s)) / 256;
    lThreshold = (lThreshold * s + 254 * (257 - s)) / 256;
  } else {
    n          = uThreshold;
  }

  // Pass all the derived values to the eye-rendering function:
  drawEye(eyeIndex, iScale, eyeX, eyeY, n, lThreshold);

  if(eyeIndex == (NUM_EYES - 1)) {
    user_loop(); // Call user code after rendering last eye
  }
}

// AUTONOMOUS IRIS SCALING (if no photocell or dial) -----------------------

#if !defined(LIGHT_PIN) || (LIGHT_PIN < 0)

// Autonomous iris motion uses a fractal behavior to similate both the major
// reaction of the eye plus the continuous smaller adjustments that occur.

uint16_t oldIris = (IRIS_MIN + IRIS_MAX) / 2, newIris;

void split( // Subdivides motion path into two sub-paths w/randimization
  int16_t  startValue, // Iris scale value (IRIS_MIN to IRIS_MAX) at start
  int16_t  endValue,   // Iris scale value at end
  uint32_t startTime,  // micros() at start
  int32_t  duration,   // Start-to-end time, in microseconds
  int16_t  range) {    // Allowable scale value variance when subdividing

  if(range >= 8) {     // Limit subdvision count, because recursion
    range    /= 2;     // Split range & time in half for subdivision,
    duration /= 2;     // then pick random center point within range:
    int16_t  midValue = (startValue + endValue - range) / 2 + random(range);
    uint32_t midTime  = startTime + duration;
    split(startValue, midValue, startTime, duration, range); // First half
    split(midValue  , endValue, midTime  , duration, range); // Second half
  } else {             // No more subdivisons, do iris motion...
    int32_t dt;        // Time (micros) since start of motion
    int16_t v;         // Interim value
    while((dt = (micros() - startTime)) < duration) {
      v = startValue + (((endValue - startValue) * dt) / duration);
      if(v < IRIS_MIN)      v = IRIS_MIN; // Clip just in case
      else if(v > IRIS_MAX) v = IRIS_MAX;
      frame(v);        // Draw frame w/interim iris scale value
    }
  }
}

#endif // !LIGHT_PIN

// MAIN LOOP -- runs continuously after setup() ----------------------------

void loop() {

#if defined(LIGHT_PIN) && (LIGHT_PIN >= 0) // Interactive iris

  int16_t v = analogRead(LIGHT_PIN);       // Raw dial/photocell reading
#ifdef LIGHT_PIN_FLIP
  v = 1023 - v;                            // Reverse reading from sensor
#endif
  if(v < LIGHT_MIN)      v = LIGHT_MIN;  // Clamp light sensor range
  else if(v > LIGHT_MAX) v = LIGHT_MAX;
  v -= LIGHT_MIN;  // 0 to (LIGHT_MAX - LIGHT_MIN)
#ifdef LIGHT_CURVE  // Apply gamma curve to sensor input?
  v = (int16_t)(pow((double)v / (double)(LIGHT_MAX - LIGHT_MIN),
    LIGHT_CURVE) * (double)(LIGHT_MAX - LIGHT_MIN));
#endif
  // And scale to iris range (IRIS_MAX is size at LIGHT_MIN)
  v = map(v, 0, (LIGHT_MAX - LIGHT_MIN), IRIS_MAX, IRIS_MIN);
#ifdef IRIS_SMOOTH // Filter input (gradual motion)
  static int16_t irisValue = (IRIS_MIN + IRIS_MAX) / 2;
  irisValue = ((irisValue * 15) + v) / 16;
  frame(irisValue);
#else // Unfiltered (immediate motion)
  frame(v);
#endif // IRIS_SMOOTH

#else  // Autonomous iris scaling -- invoke recursive function

  newIris = random(IRIS_MIN, IRIS_MAX);
  split(oldIris, newIris, micros(), 10000000L, IRIS_MAX - IRIS_MIN);
  oldIris = newIris;

#endif // LIGHT_PIN
}
