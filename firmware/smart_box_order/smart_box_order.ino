/*
 * Smart box: RFID + servo + load cell alarm + web-driven enrollment
 * Each authorized UID unlocks once; after a successful open it is removed from EEPROM
 * so the same card cannot open again (protects other orders).
 * Serial @ 9600:
 *   ADD 001A6182     — register 4-byte UID (8 hex digits)
 *   CLEAR / CLR      — remove all authorized cards
 *   LIST             — print AUTH lines (debug)
 */

#include <EEPROM.h>
#include <string.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Servo.h>
#include <HX711.h>

#define SS_PIN 10
#define RST_PIN 9
#define SERVO_PIN 8

#define RED_PIN 7
#define GREEN_PIN 6
#define BUZZER_PIN 4

#define LOADCELL_DOUT_PIN 3
#define LOADCELL_SCK_PIN 2

#define MAX_UID_SLOTS 16
/* Bumped to wipe EEPROM UIDs from older sketches (single-use rule + fresh list). */
#define EEPROM_MAGIC 0x5BU
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_COUNT 1
#define EEPROM_ADDR_UIDS 2

MFRC522 rfid(SS_PIN, RST_PIN);
Servo myServo;
HX711 scale;

byte allowedUIDs[MAX_UID_SLOTS][4];
byte uidCount = 0;

bool isLocked = true;
bool enrollMode = false;

long scaleDefault = 0;
long scaleRange = 100000;

char serialLine[48];
byte serialIdx = 0;

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  return -1;
}

bool parseUidHex8(const char *s, byte *out) {
  for (byte i = 0; i < 4; i++) {
    int hi = hexNibble(s[i * 2]);
    int lo = hexNibble(s[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (byte)((hi << 4) | lo);
  }
  return s[8] == '\0' || s[8] == ' ' || s[8] == '\r';
}

int findUidIndex(const byte *uid) {
  for (byte i = 0; i < uidCount; i++) {
    bool match = true;
    for (byte j = 0; j < 4; j++) {
      if (allowedUIDs[i][j] != uid[j]) {
        match = false;
        break;
      }
    }
    if (match) return (int)i;
  }
  return -1;
}

void removeUidFromList(const byte *uid) {
  int ix = findUidIndex(uid);
  if (ix < 0) return;
  for (byte i = (byte)ix; i + 1 < uidCount; i++) {
    memcpy(allowedUIDs[i], allowedUIDs[i + 1], 4);
  }
  if (uidCount > 0) uidCount--;
  saveUIDsToEEPROM();
  Serial.println("OK:UID_REVOKED");
}

void loadUIDsFromEEPROM() {
  if (EEPROM.read(EEPROM_ADDR_MAGIC) != EEPROM_MAGIC) {
    uidCount = 0;
    EEPROM.update(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
    EEPROM.update(EEPROM_ADDR_COUNT, 0);
    return;
  }
  uidCount = EEPROM.read(EEPROM_ADDR_COUNT);
  if (uidCount > MAX_UID_SLOTS) uidCount = 0;
  for (byte i = 0; i < uidCount; i++) {
    for (byte j = 0; j < 4; j++) {
      allowedUIDs[i][j] = EEPROM.read(EEPROM_ADDR_UIDS + i * 4 + j);
    }
  }
}

void saveUIDsToEEPROM() {
  EEPROM.update(EEPROM_ADDR_COUNT, uidCount);
  for (byte i = 0; i < uidCount; i++) {
    for (byte j = 0; j < 4; j++) {
      EEPROM.update(EEPROM_ADDR_UIDS + i * 4 + j, allowedUIDs[i][j]);
    }
  }
}

void handleCommand(char *line) {
  while (*line == ' ') line++;

  if (strncmp(line, "ADD ", 4) == 0) {
    const char *p = line + 4;
    while (*p == ' ') p++;
    byte uid[4];
    if (!parseUidHex8(p, uid)) {
      Serial.println("ERR:BAD");
      return;
    }
    if (findUidIndex(uid) >= 0) {
      Serial.println("ERR:DUPLICATE");
      return;
    }
    if (uidCount >= MAX_UID_SLOTS) {
      Serial.println("ERR:FULL");
      return;
    }
    memcpy(allowedUIDs[uidCount], uid, 4);
    uidCount++;
    saveUIDsToEEPROM();
    Serial.println("OK:ADD");
    return;
  }

  if (strcmp(line, "CLEAR") == 0 || strcmp(line, "CLR") == 0) {
    uidCount = 0;
    EEPROM.update(EEPROM_ADDR_COUNT, 0);
    Serial.println("OK:CLEAR");
    return;
  }

  if (strcmp(line, "LIST") == 0) {
    Serial.print("AUTH:");
    Serial.println(uidCount);
    for (byte i = 0; i < uidCount; i++) {
      for (byte j = 0; j < 4; j++) {
        if (allowedUIDs[i][j] < 16) Serial.print('0');
        Serial.print(allowedUIDs[i][j], HEX);
      }
      Serial.println();
    }
    return;
  }

  if (strcmp(line, "ENROLL ON") == 0 || strcmp(line, "ENROLL 1") == 0) {
    enrollMode = true;
    Serial.println("OK:ENROLL_ON");
    return;
  }
  if (strcmp(line, "ENROLL OFF") == 0 || strcmp(line, "ENROLL 0") == 0) {
    enrollMode = false;
    Serial.println("OK:ENROLL_OFF");
    return;
  }
}

void readSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serialLine[serialIdx] = '\0';
      serialIdx = 0;
      if (serialLine[0] != '\0') handleCommand(serialLine);
      continue;
    }
    if (serialIdx < sizeof(serialLine) - 1) {
      serialLine[serialIdx++] = c;
    } else {
      serialIdx = 0;
    }
  }
}

void setup() {
  Serial.begin(9600);
  SPI.begin();
  rfid.PCD_Init();

  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(RED_PIN, LOW);
  digitalWrite(GREEN_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  myServo.attach(SERVO_PIN);
  myServo.write(0);

  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);

  loadUIDsFromEEPROM();

  Serial.println("System starting...");
  Serial.println("Calibrating load cell...");

  delay(1000);

  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += scale.read();
    delay(100);
  }
  scaleDefault = sum / 10;

  Serial.print("Scale default: ");
  Serial.println(scaleDefault);

  Serial.println("Ready. Enroll via web (ADD ...) or scan if already authorized.");
}

void loop() {
  readSerialCommands();

  // While enrolling from the web app, skip the load-cell alarm. Otherwise a noisy cell
  // triggers alarm() — its long delays() starve the loop and the RC522 never sees the card.
  if (isLocked && !enrollMode && checkForce()) {
    alarm();
  }

  // Same pattern as the stock MFRC522 examples / your original sketch — requiring two
  // IsNewCardPresent() successes in a row often fails (2nd call goes false → no read).
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    Serial.print("Card UID: ");
    for (byte i = 0; i < rfid.uid.size; i++) {
      Serial.print(rfid.uid.uidByte[i], HEX);
      if (i < rfid.uid.size - 1) Serial.print(' ');
    }
    Serial.println();

    if (enrollMode) {
      if (rfid.uid.size == 4) {
        Serial.println("ENROLL:OK");
      } else {
        Serial.println("ENROLL:BADSIZE");
      }
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    } else {
      bool authorized = false;
      if (rfid.uid.size == 4 && uidCount > 0) {
        authorized = (findUidIndex(rfid.uid.uidByte) >= 0);
      }

      if (authorized) {
        Serial.println("Access granted");
        unlockBox();
        // One-time access: drop this card from authorized list so it cannot unlock again.
        removeUidFromList(rfid.uid.uidByte);
      } else {
        Serial.println("Access denied");
        denyAccess();
      }

      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
  }
}

bool checkForce() {
  // Avoid blocking the whole loop inside HX711 read() while WAITING for the chip;
  // that starves the RFID polling. Only read when DOUT says a conversion is ready.
  if (!scale.is_ready()) {
    return false;
  }

  long reading = scale.read();

  static unsigned long lastLoadPrint = 0;
  if (millis() - lastLoadPrint >= 1000UL) {
    lastLoadPrint = millis();
    Serial.print("Load cell reading: ");
    Serial.println(reading);
  }

  if (reading > (scaleDefault + scaleRange) || reading < (scaleDefault - scaleRange)) {
    return true;
  }
  return false;
}

void unlockBox() {
  isLocked = false;

  digitalWrite(GREEN_PIN, HIGH);
  myServo.write(180);

  delay(5000);

  myServo.write(0);
  digitalWrite(GREEN_PIN, LOW);

  isLocked = true;
  Serial.println("Box locked again");
}

void denyAccess() {
  digitalWrite(RED_PIN, HIGH);
  tone(BUZZER_PIN, 1000);
  delay(300);
  noTone(BUZZER_PIN);
  digitalWrite(RED_PIN, LOW);
}

void alarm() {
  Serial.println("Burglary alert!");

  tone(BUZZER_PIN, 1200);

  for (int i = 0; i < 10; i++) {
    digitalWrite(RED_PIN, HIGH);
    delay(100);
    digitalWrite(RED_PIN, LOW);
    delay(100);
  }

  noTone(BUZZER_PIN);
}
