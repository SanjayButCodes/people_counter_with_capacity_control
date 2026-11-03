#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>   // "LiquidCrystal I2C" by Frank de Brabander
#include <Servo.h>

const int SENSOR_A  = 2;   // IR sensor A (interrupt pin)
const int SENSOR_B  = 3;   // IR sensor B (interrupt pin)
const int RESET_BTN = 4;
const int BUZZER    = 5;
const int RELAY_PIN = 6;   // relay module IN (controls the fan)

// RGB LED legs are in G, B, R order, so the pins are mapped like this:
const int LED_R     = A0;  // red leg   -> A0
const int LED_G     = 7;   // green leg -> D7
const int LED_B     = 8;   // blue leg  -> D8 (kept off)

const int SERVO_PIN = 9;
// 3rd DIP switch (D11) is broken and not used, so only 3 switches remain
const int DIP0 = 13;       // switch 1 (value 1)
const int DIP1 = 12;       // switch 2 (value 2)
const int DIP3 = 10;       // switch 4 (value 4)  -> capacity range 0..7
// LCD I2C: SDA = A4, SCL = A5, VCC = 5V, GND = GND

const bool COMMON_ANODE = true;       // RGB LED: longest leg to 5V
const bool RELAY_ACTIVE_LOW = true;   // set false if relay works inverted

const int GATE_OPEN   = 90;           // swap if the arm moves the wrong way
const int GATE_CLOSED = 0;

const unsigned long TIMEOUT = 2000;   // ms allowed between sensor A and B

Servo gateServo;
int gatePos = GATE_OPEN;

LiquidCrystal_I2C *lcd = nullptr;

int count = 0;
int capacity = 0;          // 0 = no limit
bool buttonUsable = true;
bool buttonHeld = false;
bool displayOk = false;

volatile bool hitA = false, hitB = false;
volatile unsigned long tA = 0, tB = 0;
bool waitClear = false;

// animation state: 0 none, 1 entering, -1 leaving, 2 reset, 3 full/denied
int animType = 0;
unsigned long animStart = 0;
unsigned long lastFrame = 0;

int beepsLeft = 0;
unsigned long beepNext = 0;

// ---------- LCD frame buffer ----------
char rows[2][16];
char shadow[2][16];
bool forceFull = true;
bool flashNow = false;
bool backlightOn = true;

// custom characters
uint8_t chWalk0[8] = {B01110, B01110, B00100, B01110, B10101, B00100, B01010, B10001};
uint8_t chWalk1[8] = {B01110, B01110, B10101, B01110, B00100, B00100, B01010, B01010};
uint8_t chArrR[8]  = {B00000, B00100, B00010, B11111, B00010, B00100, B00000, B00000};
uint8_t chArrL[8]  = {B00000, B00100, B01000, B11111, B01000, B00100, B00000, B00000};
const char C_WALK0 = 0, C_WALK1 = 1, C_ARR_R = 2, C_ARR_L = 3, C_BLOCK = (char)255;

// ---------- software PWM for the RGB LED (pins are not hardware PWM) ----------
// Timer2 fires ~4000 times a second; 32 brightness steps -> ~125 Hz, no visible flicker.
volatile uint8_t pwmR = 0, pwmG = 0;   // brightness 0..32
volatile uint8_t pwmPhase = 0;

ISR(TIMER2_COMPA_vect) {
  pwmPhase = (pwmPhase + 1) & 31;
  bool rOn = pwmR > pwmPhase;
  bool gOn = pwmG > pwmPhase;
  digitalWrite(LED_R, COMMON_ANODE ? !rOn : rOn);
  digitalWrite(LED_G, COMMON_ANODE ? !gOn : gOn);
}

void startLedPwm() {
  noInterrupts();
  TCCR2A = (1 << WGM21);       // CTC mode
  TCCR2B = (1 << CS22);        // prescaler 64 -> 250 kHz
  OCR2A  = 61;                 // 250 kHz / 62 = ~4 kHz
  TCNT2  = 0;
  TIMSK2 = (1 << OCIE2A);
  interrupts();
}

void isrA() { if (!hitA) { tA = micros(); hitA = true; } }
void isrB() { if (!hitB) { tB = micros(); hitB = true; } }

void clearHits() {
  noInterrupts();
  hitA = false;
  hitB = false;
  interrupts();
}

void startBeep(int times) {
  beepsLeft = times * 2;
  beepNext = millis();
}

void updateBeep() {
  if (beepsLeft > 0 && millis() >= beepNext) {
    digitalWrite(BUZZER, (beepsLeft % 2 == 0) ? HIGH : LOW);
    beepsLeft--;
    beepNext = millis() + 80;
  }
}

bool i2cPresent(byte addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool isFull() {
  return (capacity > 0 && count >= capacity);
}

// ---------- Relay (fan) ----------

void setRelay(bool on) {
  digitalWrite(RELAY_PIN, (on != RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

// ---------- RGB LED (green -> yellow -> red as the room fills) ----------

// r and g are brightness 0..255
void writeColor(int r, int g) {
  pwmR = (r + 7) / 8;     // 0..32
  pwmG = (g + 7) / 8;
}

void updateLed() {
  unsigned long t = millis() - animStart;

  // entry denied: red blinks fast
  if (animType == 3 && t < 1200) {
    writeColor(((t / 150) % 2) == 0 ? 255 : 0, 0);
    return;
  }

  // target colour from how full the room is
  int targetR, targetG;
  if (capacity > 0) {
    int p = (int)min(100L, (long)count * 100L / capacity);   // 0..100 %
    if (p < 50) { targetR = p * 255 / 50;  targetG = 255; }              // green -> yellow
    else        { targetR = 255;           targetG = (100 - p) * 255 / 50; }  // yellow -> red
  } else {
    targetR = 0;
    targetG = 255;                                           // no limit: stay green
  }

  // fade smoothly toward the target
  static int curR = 0, curG = 0;
  static unsigned long lastFade = 0;
  if (millis() - lastFade >= 10) {
    lastFade = millis();
    if (curR < targetR) curR = min(curR + 4, targetR);
    else if (curR > targetR) curR = max(curR - 4, targetR);
    if (curG < targetG) curG = min(curG + 4, targetG);
    else if (curG > targetG) curG = max(curG - 4, targetG);
  }
  writeColor(curR, curG);
}

// DIP switch ON = pin pulled to GND = LOW
int readCapacity() {
  int v = 0;
  if (digitalRead(DIP0) == LOW) v |= 1;
  if (digitalRead(DIP1) == LOW) v |= 2;
  if (digitalRead(DIP3) == LOW) v |= 4;
  return v;
}

// Moves the gate smoothly toward open or closed
void updateGate() {
  static unsigned long last = 0;
  if (millis() - last < 15) return;
  last = millis();

  int target = isFull() ? GATE_CLOSED : GATE_OPEN;
  int newPos = gatePos;
  if (newPos < target) newPos = (newPos + 2 > target) ? target : newPos + 2;
  else if (newPos > target) newPos = (newPos - 2 < target) ? target : newPos - 2;

  if (newPos != gatePos) {
    gatePos = newPos;
    gateServo.write(gatePos);
  }
}

void countChanged(int type) {  // 1 enter, -1 leave, 2 reset, 3 full
  animType = type;
  animStart = millis();
}

// ---------- LCD helpers ----------

void clearBuf() { memset(rows, ' ', sizeof(rows)); }

void putStr(int row, int col, const char *s) {
  for (; *s; s++, col++) {
    if (col >= 16) break;
    if (col >= 0) rows[row][col] = *s;
  }
}

void putCentered(int row, const char *s) {
  putStr(row, (16 - (int)strlen(s)) / 2, s);
}

void setBacklight(bool on) {
  if (on == backlightOn) return;
  backlightOn = on;
  if (on) lcd->backlight(); else lcd->noBacklight();
}

// Only rewrites cells that changed, so it stays smooth and flicker-free
void show() {
  for (int r = 0; r < 2; r++) {
    for (int c = 0; c < 16; c++) {
      if (forceFull || rows[r][c] != shadow[r][c]) {
        lcd->setCursor(c, r);
        lcd->write((uint8_t)rows[r][c]);
        shadow[r][c] = rows[r][c];
      }
    }
  }
  forceFull = false;
}

// ---------- drawing ----------

void drawIdle() {
  clearBuf();
  putCentered(0, "IIT ROPAR");

  // light bar sweeping through the empty cells of the title row
  int p = (int)((millis() / 70) % 22) - 3;
  for (int k = 0; k < 2; k++) {
    int c = p + k;
    if (c >= 0 && c < 16 && rows[0][c] == ' ') rows[0][c] = C_BLOCK;
  }

  char line[20];
  bool statusPhase = ((millis() / 3000) % 2) == 0;

  if (isFull() || statusPhase) {
    if (isFull() && ((millis() / 500) % 2)) {
      strcpy(line, "GATE CLOSED-FULL");
    } else if (capacity > 0) {
      sprintf(line, "INSIDE: %d / %d", count, capacity);
    } else {
      sprintf(line, "INSIDE: %d", count);
    }
    putCentered(1, line);
  } else {
    // endless walking stick figure
    int fx = (int)((millis() % 3000) / 170) - 1;
    if (fx >= 0 && fx < 16) rows[1][fx] = ((millis() / 170) % 2) ? C_WALK1 : C_WALK0;
  }
}

void drawActive(unsigned long t) {
  clearBuf();
  char buf[16];
  sprintf(buf, "INSIDE: %d", count);
  putStr(0, 0, buf);
  putStr(0, animType == 1 ? 14 : 13, animType == 1 ? "IN" : "OUT");

  // streaming arrows
  int s = (t / 90) % 3;
  int pos = (animType == 1) ? s : 2 - s;
  for (int c = 0; c < 16; c++) {
    if (c % 3 == pos) rows[1][c] = (animType == 1) ? C_ARR_R : C_ARR_L;
  }

  if (t < 100) flashNow = true;
}

void drawReset(unsigned long t) {
  clearBuf();
  putCentered(0, "RESET");
  putCentered(1, "COUNT = 0");
  if (t < 100 || (t > 300 && t < 400) || (t > 600 && t < 700)) flashNow = true;
}

void drawFull(unsigned long t) {
  clearBuf();
  putCentered(0, "FULL");
  putCentered(1, "GATE CLOSED");
  if (t < 100 || (t > 250 && t < 350) || (t > 500 && t < 600) || (t > 750 && t < 850)) flashNow = true;
}

void drawFrame() {
  unsigned long t = millis() - animStart;
  unsigned long dur = 1500UL;
  if (animType == 2) dur = 900UL;
  if (animType == 3) dur = 1200UL;
  bool active = (animType != 0) && (t < dur);

  flashNow = false;
  if (!active) drawIdle();
  else if (animType == 2) drawReset(t);
  else if (animType == 3) drawFull(t);
  else drawActive(t);

  show();
  setBacklight(!flashNow);
}

void splash() {
  for (int x = -1; x < 17; x++) {
    clearBuf();
    putCentered(0, "PEOPLE COUNTER");
    if (x >= 0 && x < 16) rows[1][x] = (x % 2) ? C_WALK1 : C_WALK0;
    show();
    delay(90);
  }
  delay(300);
}

// ---------- setup / loop ----------

void setup() {
  Serial.begin(9600);
  pinMode(SENSOR_A, INPUT);
  pinMode(SENSOR_B, INPUT);
  pinMode(RESET_BTN, INPUT_PULLUP);
  pinMode(DIP0, INPUT_PULLUP);
  pinMode(DIP1, INPUT_PULLUP);
  pinMode(DIP3, INPUT_PULLUP);
  pinMode(BUZZER, OUTPUT);
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  digitalWrite(BUZZER, LOW);
  digitalWrite(LED_B, COMMON_ANODE ? HIGH : LOW);   // blue always off
  writeColor(0, 0);
  startLedPwm();

  // Relay OFF level first, then make it an output (no click at boot)
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(RELAY_PIN, OUTPUT);

  gateServo.attach(SERVO_PIN);
  gateServo.write(gatePos);
  capacity = readCapacity();

  Wire.begin();

  byte addr = 0;
  if (i2cPresent(0x27)) addr = 0x27;
  else if (i2cPresent(0x3F)) addr = 0x3F;

  if (addr == 0) {
    Serial.println("LCD NOT FOUND - check SDA/SCL/VCC/GND wiring");
    startBeep(5);  // 5 beeps = display not detected
  } else {
    Serial.print("LCD found at 0x");
    Serial.println(addr, HEX);
    lcd = new LiquidCrystal_I2C(addr, 16, 2);
    lcd->init();
    lcd->backlight();
    Wire.setClock(400000);
    lcd->createChar(0, chWalk0);
    lcd->createChar(1, chWalk1);
    lcd->createChar(2, chArrR);
    lcd->createChar(3, chArrL);
    lcd->clear();
    displayOk = true;
    forceFull = true;
    splash();
    forceFull = true;
    startBeep(1);  // 1 beep = all good
  }

  buttonUsable = (digitalRead(RESET_BTN) == HIGH);
  if (!buttonUsable) {
    Serial.println("Reset button looks stuck - check wiring. Ignoring it.");
  }

  // IR sensors output LOW when they detect something
  attachInterrupt(digitalPinToInterrupt(SENSOR_A), isrA, FALLING);
  attachInterrupt(digitalPinToInterrupt(SENSOR_B), isrB, FALLING);
}

void loop() {
  updateBeep();
  updateGate();
  updateLed();

  // Fan runs only while someone is inside
  setRelay(count > 0);

  // read the DIP switches a few times per second
  static unsigned long lastDip = 0;
  if (millis() - lastDip >= 200) {
    lastDip = millis();
    capacity = readCapacity();
  }

  if (displayOk && millis() - lastFrame >= 100) {
    lastFrame = millis();
    drawFrame();
  }

  // Reset button
  if (buttonUsable) {
    if (digitalRead(RESET_BTN) == LOW && !buttonHeld) {
      delay(30);
      if (digitalRead(RESET_BTN) == LOW) {
        buttonHeld = true;
        count = 0;
        countChanged(2);
        startBeep(3);
        clearHits();
      }
    } else if (digitalRead(RESET_BTN) == HIGH) {
      buttonHeld = false;
    }
  }

  // Direction detection from the interrupt timestamps
  bool aHit, bHit;
  unsigned long ta, tb;
  noInterrupts();
  aHit = hitA; bHit = hitB; ta = tA; tb = tB;
  interrupts();

  bool bothClear = (digitalRead(SENSOR_A) == HIGH) && (digitalRead(SENSOR_B) == HIGH);

  if (waitClear) {
    if (bothClear) { clearHits(); waitClear = false; }
  } else if (aHit && bHit) {
    long diff = (long)(tb - ta);
    if (diff > 0) {                       // A first, then B = entering
      if (isFull()) {
        countChanged(3);                  // room is full: do not count
        startBeep(4);
      } else {
        count++;
        countChanged(1);
        startBeep(1);
      }
    } else if (diff < 0) {                // B first, then A = leaving
      if (count > 0) count--;
      countChanged(-1);
      startBeep(2);
    }
    clearHits();
    waitClear = true;
  } else if (aHit || bHit) {
    unsigned long first = aHit ? ta : tb;
    if (micros() - first > TIMEOUT * 1000UL) clearHits();  // half crossing
  }
}