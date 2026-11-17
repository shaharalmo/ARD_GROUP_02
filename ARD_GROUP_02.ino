/*
  SMART ELECTION STATION
  Arduino Uno + Fingerprint Sensor + IR Remote

  D2  - Fingerprint sensor TX
  D3  - Fingerprint sensor RX
  D4  - Green LED
  D5  - Red LED
  D6  - Servo signal
  D7  - Finish voting button
  D8  - IR receiver signal

  LCD I2C:
  SDA=A4, SCL=A5 (plus 5V and GND)

  Serial Monitor: 9600 baud
*/

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>
#include <IRremote.hpp>

// ========== PIN DEFINITIONS ==========

const byte GREEN_LED_PIN = 4;
const byte RED_LED_PIN = 5;
const byte SERVO_PIN = 6;
const byte FINISH_BUTTON_PIN = 7;
const byte IR_PIN = 8;

// ========== SETTINGS ==========

const bool ENABLE_SERVO = true;
const int GATE_STOP = 90;
const int GATE_OPEN_SPEED = 75;
const int GATE_CLOSE_SPEED = 105;
const unsigned long GATE_RUN_MS = 700;

// Assumes the gate starts CLOSED when Arduino starts.
bool gateOpenCommanded = false;
bool gateMoving = false;
unsigned long gateMoveStartedAt = 0;

const unsigned long DEBOUNCE_MS = 35;
const unsigned long NOTICE_MS = 1800;
const unsigned long SCAN_INTERVAL_MS = 150;
const unsigned long MANAGER_COOLDOWN_MS = 700;

// ========== IR REMOTE CONFIGURATION ==========

const uint16_t MANAGER_ADDRESS = 0x0;
const uint16_t MANAGER_COMMAND = 0x1c;

const bool REMOTE_CONFIGURED = true;

// ========== COMPONENTS ==========

LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo gate;

SoftwareSerial fingerSerial(2, 3);
Adafruit_Fingerprint finger(&fingerSerial);

bool sensorReady = false;
bool fingerHandled = false;

unsigned long lastScanAt = 0;
unsigned long lastManagerCommandAt = 0;
bool managerCommandSeen = false;

// ========== VOTER DATABASE ==========

struct Voter {
  uint16_t id;
  const char* name;
  const char* idNumber;
  bool voted;
};

Voter voters[] = {
  {1, "Adi Aviv", "000000001", false},
  {2, "Shahar Almozlinos", "000000002", false},
  {3, "Noam Ben Ami", "000000003", false},
  {4, "Noam Gitit", "000000004", false}
};

const int NUM_VOTERS =
  sizeof(voters) / sizeof(voters[0]);

// Index of the voter currently voting.
// -1 means that nobody is voting.
int activeVoter = -1;

// ========== STATION STATES ==========

enum StationState {
  CLOSED,
  READY,
  VOTING
};

StationState stationState = CLOSED;

bool closeAfterVote = false;
bool noticeActive = false;
unsigned long noticeStartedAt = 0;

// ========== BUTTON DEBOUNCING ==========

struct Button {
  byte pin;
  byte lastRaw;
  byte stable;
  unsigned long lastChangeAt;

  explicit Button(byte inputPin)
    : pin(inputPin),
      lastRaw(HIGH),
      stable(HIGH),
      lastChangeAt(0) {}

  void begin() {
    pinMode(pin, INPUT_PULLUP);
    lastRaw = digitalRead(pin);
    stable = lastRaw;
    lastChangeAt = millis();
  }

  bool wasPressed(unsigned long now) {
    byte raw = digitalRead(pin);

    if (raw != lastRaw) {
      lastRaw = raw;
      lastChangeAt = now;
    }

    if (raw != stable &&
        (unsigned long)(now - lastChangeAt)
          >= DEBOUNCE_MS) {
      stable = raw;
      return stable == LOW;
    }

    return false;
  }
};

Button finishButton(FINISH_BUTTON_PIN);

// ========== DATABASE SEARCH ==========

int findVoter(uint16_t fingerprintID) {
  for (int i = 0; i < NUM_VOTERS; i++) {
    if (voters[i].id == fingerprintID) {
      return i;
    }
  }

  return -1;
}

// ========== LCD AND LED ==========

void printScreen(const __FlashStringHelper* line1,
                 const __FlashStringHelper* line2) {
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print(line1);

  lcd.setCursor(0, 1);
  lcd.print(line2);
}

void setLeds(bool green, bool red) {
  digitalWrite(GREEN_LED_PIN,
               green ? HIGH : LOW);

  digitalWrite(RED_LED_PIN,
               red ? HIGH : LOW);
}

// Start a timed movement, then stop automatically in updateGateMotion().
void updateGate() {
  if (!ENABLE_SERVO) return;

  bool shouldBeOpen = (stationState == VOTING);
  if (shouldBeOpen == gateOpenCommanded) return;

  gateOpenCommanded = shouldBeOpen;
  gate.write(shouldBeOpen ? GATE_OPEN_SPEED : GATE_CLOSE_SPEED);
  gateMoveStartedAt = millis();
  gateMoving = true;
  Serial.println(shouldBeOpen ? F("[GATE] Opening") : F("[GATE] Closing"));
}

void updateGateMotion() {
  if (!ENABLE_SERVO || !gateMoving) return;

  if ((unsigned long)(millis() - gateMoveStartedAt) >= GATE_RUN_MS) {
    gate.write(GATE_STOP);
    gateMoving = false;
    Serial.println(F("[GATE] Stopped"));
  }
}

void showState() {
  noticeActive = false;
  setLeds(stationState == VOTING, false);

  if (stationState == CLOSED) {
    printScreen(F("Station CLOSED"),
                F("Manager: Remote"));
  }

  else if (stationState == READY) {
    printScreen(F("Station OPEN"),
                F("Place finger"));
  }

  else if (closeAfterVote) {
    printScreen(F("Closing pending"),
                F("Finish voting"));
  }

  else if (activeVoter >= 0) {
    lcd.clear();
    lcd.setCursor(0, 0);

    lcd.print(F("Hi "));

    const char* name = voters[activeVoter].name;

    for (byte i = 0;
         i < 13 && name[i] != '\0';
         i++) {
      lcd.print(name[i]);
    }

    lcd.setCursor(0, 1);
    lcd.print(F("Press FINISH"));
  }
}

void showNotice(const __FlashStringHelper* line1,
                const __FlashStringHelper* line2,
                bool denied) {
  printScreen(line1, line2);

  setLeds(!denied && stationState == VOTING,
          denied);

  noticeActive = true;
  noticeStartedAt = millis();
}

// ========== CHANGE STATION STATE ==========

void changeState(StationState newState) {
  stationState = newState;

  updateGate();
  showState();

  Serial.print(F("[STATE] "));

  if (newState == CLOSED) {
    Serial.println(F("CLOSED"));
  }

  else if (newState == READY) {
    Serial.println(F("READY"));
  }

  else {
    Serial.println(F("VOTING"));
  }
}

// ========== MANAGER COMMAND ==========

void handleManagerCommand() {
  unsigned long now = millis();

  if (managerCommandSeen &&
      (unsigned long)(now - lastManagerCommandAt)
        < MANAGER_COOLDOWN_MS) {
    return;
  }

  managerCommandSeen = true;
  lastManagerCommandAt = now;

  if (stationState == CLOSED) {
    if (!sensorReady) {
      Serial.println(F("[ERROR] Fingerprint offline"));

      showNotice(F("Sensor ERROR"),
                 F("Check sensor"),
                 true);
      return;
    }

    closeAfterVote = false;

    Serial.println(F("[MANAGER] Opening station"));
    changeState(READY);
  }

  else if (stationState == READY) {
    Serial.println(F("[MANAGER] Closing station"));
    changeState(CLOSED);
  }

  else {
    closeAfterVote = true;

    Serial.println(
      F("[MANAGER] Close after current vote"));

    showState();
  }
}

// ========== READ IR REMOTE ==========

void pollRemote() {
  if (!IrReceiver.decode()) {
    return;
  }

  uint16_t receivedAddress =
    IrReceiver.decodedIRData.address;

  uint16_t receivedCommand =
    IrReceiver.decodedIRData.command;

  bool isRepeat =
    (IrReceiver.decodedIRData.flags &
      IRDATA_FLAGS_IS_REPEAT) != 0;

  bool isOverflow =
    (IrReceiver.decodedIRData.flags &
      IRDATA_FLAGS_WAS_OVERFLOW) != 0;

  bool isKnown =
    IrReceiver.decodedIRData.protocol != UNKNOWN;

  IrReceiver.resume();

  if (isRepeat || isOverflow || !isKnown) {
    return;
  }

  Serial.print(F("[IR] Address: 0x"));
  Serial.println(receivedAddress, HEX);

  Serial.print(F("[IR] Command: 0x"));
  Serial.println(receivedCommand, HEX);

  if (!REMOTE_CONFIGURED) {
    Serial.println(
      F("[IR] Setup mode: command not enabled"));
    return;
  }

  if (receivedAddress == MANAGER_ADDRESS &&
      receivedCommand == MANAGER_COMMAND) {
    handleManagerCommand();
  }
  else {
    Serial.println(F("[IR] Unauthorized command"));
  }
}

// ========== IDENTIFY VOTER ==========

void handleVoterRequest(int fingerprintID) {

  if (stationState != READY) {
    Serial.println(F("[DENIED] Station unavailable"));

    showNotice(F("Station closed"),
               F("Or busy"),
               true);
    return;
  }

  int index = findVoter(fingerprintID);

  if (index == -1) {
    Serial.println(F("[DENIED] Unknown voter"));

    showNotice(F("Unknown voter"),
               F("Access denied"),
               true);
    return;
  }

  if (voters[index].voted) {
    Serial.print(F("[DENIED] Already voted: "));
    Serial.println(voters[index].id);

    showNotice(F("Already voted"),
               F("Access denied"),
               true);
    return;
  }

  activeVoter = index;

  Serial.print(F("[GRANTED] ID: "));
  Serial.println(voters[index].id);

  Serial.print(F("[VOTER] Name: "));
  Serial.println(voters[index].name);

  changeState(VOTING);
}

// ========== FINGERPRINT SCANNING ==========

void pollFingerprint() {
  if (!sensorReady || stationState != READY) {
    return;
  }

  unsigned long now = millis();

  if ((unsigned long)(now - lastScanAt)
       < SCAN_INTERVAL_MS) {
    return;
  }

  // Avoid beginning a scan while IR is receiving.
  if (!IrReceiver.isIdle()) {
    return;
  }

  lastScanAt = now;

  uint8_t result = finger.getImage();

  if (result == FINGERPRINT_NOFINGER) {
    fingerHandled = false;
    return;
  }

  // Avoid processing one held finger repeatedly.
  if (fingerHandled) {
    return;
  }

  if (result != FINGERPRINT_OK) {
    Serial.print(F("[FINGER] Capture error: "));
    Serial.println(result);
    return;
  }

  fingerHandled = true;

  result = finger.image2Tz();

  if (result != FINGERPRINT_OK) {
    Serial.print(F("[FINGER] Conversion error: "));
    Serial.println(result);

    showNotice(F("Scan failed"),
               F("Try again"),
               true);
    return;
  }

  result = finger.fingerFastSearch();

  if (result == FINGERPRINT_NOTFOUND ||
      result == FINGERPRINT_NOMATCH) {
    handleVoterRequest(-1);
    return;
  }

  if (result != FINGERPRINT_OK) {
    Serial.print(F("[FINGER] Search error: "));
    Serial.println(result);

    showNotice(F("Sensor error"),
               F("Try again"),
               true);
    return;
  }

  Serial.print(F("[FINGER] Found ID: "));
  Serial.println(finger.fingerID);

  Serial.print(F("[FINGER] Confidence: "));
  Serial.println(finger.confidence);

  handleVoterRequest(finger.fingerID);
}

// ========== FINISH VOTING ==========

void handleFinish() {
  if (stationState != VOTING ||
      activeVoter < 0) {
    Serial.println(F("[IGNORED] No active vote"));
    return;
  }

  voters[activeVoter].voted = true;

  Serial.print(F("[VOTE] Completed by ID: "));
  Serial.println(voters[activeVoter].id);

  activeVoter = -1;

  bool mustClose = closeAfterVote;
  closeAfterVote = false;

  changeState(mustClose ? CLOSED : READY);

  showNotice(F("Vote recorded"),
             mustClose
                ? F("Station CLOSED")
                : F("Thank you"),
             false);
}

// ========== SETUP ==========
void setup() {
  Serial.begin(9600);

  finishButton.begin();

  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);

  setLeds(false, false);

  lcd.init();
  lcd.backlight();

  if (ENABLE_SERVO) {
    gate.write(GATE_STOP);
    gate.attach(SERVO_PIN);
  }

  printScreen(F("Initializing"),
              F("Fingerprint"));

  finger.begin(57600);

  sensorReady = finger.verifyPassword();

  if (sensorReady) {
    Serial.println(
      F("[BOOT] Fingerprint sensor found"));
  }
  else {
    Serial.println(
      F("[ERROR] Fingerprint sensor not found"));
  }

  IrReceiver.begin(IR_PIN, false);

  Serial.println(F("[BOOT] IR receiver ready"));

  if (!REMOTE_CONFIGURED) {
    Serial.println(
      F("[IR] Press desired manager button"));
    Serial.println(
      F("[IR] Copy Address and Command"));
  }

  changeState(CLOSED);

  if (!sensorReady) {
    showNotice(F("Sensor ERROR"),
               F("Check sensor"),
               true);
  }

  Serial.print(F("[BOOT] Number of voters: "));
  Serial.println(NUM_VOTERS);
}

// ========== MAIN LOOP ==========

void loop() {
  updateGateMotion();
  unsigned long now = millis();

  bool finishPressed =
    finishButton.wasPressed(now);

  bool wasVoting = stationState == VOTING;

  pollRemote();

  if (finishPressed) {
    handleFinish();
  }

  // Do not begin a new vote in the same cycle
  // as the completion of the previous vote.
  if (!(wasVoting && finishPressed) && !gateMoving) {
    pollFingerprint();
  }

  if (noticeActive &&
      (unsigned long)(millis() - noticeStartedAt)
        >= NOTICE_MS) {
    showState();
  }
}