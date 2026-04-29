#include <SoftwareSerial.h>

// HC-05 wiring:
// Arduino pin 10 = RX  <- HC-05 TXD
// Arduino pin 11 = TX  -> HC-05 RXD
SoftwareSerial BT(10, 11);

// L298N pins
const byte IN1 = 2;
const byte IN2 = 3;
const byte IN3 = 4;
const byte IN4 = 5;
const byte ENA = 6;
const byte ENB = 9;

// Speed settings
const byte DRIVE_SPEED = 200;
const byte TURN_SPEED  = 120;

// Safety settings
const unsigned long SAFETY_TIMEOUT_MS = 3000;
const unsigned long COMMAND_LOSS_TIMEOUT_MS = 350;

// Incoming command format from web app:
// STATE:<mask>
//
// Bit mask:
// 1 = Forward
// 2 = Backward
// 4 = Left
// 8 = Right
const byte BIT_FORWARD  = 1;
const byte BIT_BACKWARD = 2;
const byte BIT_LEFT     = 4;
const byte BIT_RIGHT    = 8;

const bool DEBUG_SERIAL = false;

char inputBuffer[24];
byte inputIndex = 0;

byte currentMask = 0;

bool motionActive = false;
bool safetyLocked = false;
unsigned long motionStartTime = 0;
unsigned long lastCommandTime = 0;

// --------------------------------------------------
// Function declarations for ArduinoDroid compatibility
// --------------------------------------------------
void readBluetoothLines();
void handleLine(const char *line);
void updateDriveControl();
void applyMask(byte mask);

void setMotorSpeeds(int leftSpeed, int rightSpeed);
void setSingleMotor(byte pinA, byte pinB, byte enablePin, int signedSpeed);

void stopMotors();
void resetMotionTimer();

bool startsWith(const char *text, const char *prefix);

// --------------------------------------------------
// Setup
// --------------------------------------------------
void setup() {
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  pinMode(ENA, OUTPUT);
  pinMode(ENB, OUTPUT);

  Serial.begin(9600);
  BT.begin(9600);

  stopMotors();

  BT.println("READY");

  if (DEBUG_SERIAL) {
    Serial.println("READY");
  }
}

// --------------------------------------------------
// Main loop
// --------------------------------------------------
void loop() {
  readBluetoothLines();
  updateDriveControl();
}

// --------------------------------------------------
// Bluetooth input handling
// --------------------------------------------------
void readBluetoothLines() {
  while (BT.available() > 0) {
    char c = BT.read();

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      inputBuffer[inputIndex] = '\0';

      if (inputIndex > 0) {
        handleLine(inputBuffer);
      }

      inputIndex = 0;
      continue;
    }

    if (inputIndex < sizeof(inputBuffer) - 1) {
      inputBuffer[inputIndex] = c;
      inputIndex++;
    } else {
      // Buffer overflow protection
      inputIndex = 0;
    }
  }
}

void handleLine(const char *line) {
  if (DEBUG_SERIAL) {
    Serial.print("RX: ");
    Serial.println(line);
  }

  if (!startsWith(line, "STATE:")) {
    return;
  }

  int value = atoi(line + 6);

  if (value < 0) {
    value = 0;
  }

  if (value > 15) {
    value = 15;
  }

  currentMask = (byte)value;
  lastCommandTime = millis();

  // STATE:0 means all buttons released
  if (currentMask == 0) {
    if (safetyLocked) {
      safetyLocked = false;
      BT.println("UNLOCKED");

      if (DEBUG_SERIAL) {
        Serial.println("UNLOCKED");
      }
    }

    resetMotionTimer();
    stopMotors();
  }
}

// --------------------------------------------------
// Driving and safety logic
// --------------------------------------------------
void updateDriveControl() {
  // No buttons pressed
  if (currentMask == 0) {
    stopMotors();
    resetMotionTimer();
    return;
  }

  // After safety lock, stay stopped until STATE:0 is received
  if (safetyLocked) {
    stopMotors();
    return;
  }

  // If Bluetooth commands stop arriving, stop the car
  if (millis() - lastCommandTime > COMMAND_LOSS_TIMEOUT_MS) {
    currentMask = 0;
    stopMotors();
    resetMotionTimer();

    if (DEBUG_SERIAL) {
      Serial.println("COMMAND TIMEOUT");
    }

    return;
  }

  // Start timing when movement begins
  if (!motionActive) {
    motionActive = true;
    motionStartTime = millis();
  }

  // 3-second safety auto-stop
  if (millis() - motionStartTime >= SAFETY_TIMEOUT_MS) {
    stopMotors();
    motionActive = false;
    safetyLocked = true;

    BT.println("SAFETY_LOCK");

    if (DEBUG_SERIAL) {
      Serial.println("SAFETY_LOCK");
    }

    return;
  }

  applyMask(currentMask);
}

void applyMask(byte mask) {
  bool forward  = (mask & BIT_FORWARD)  != 0;
  bool backward = (mask & BIT_BACKWARD) != 0;
  bool left     = (mask & BIT_LEFT)     != 0;
  bool right    = (mask & BIT_RIGHT)    != 0;

  // Conflicting throttle commands: stop safely
  if (forward && backward) {
    stopMotors();
    return;
  }

  // Forward modes
  if (forward) {
    if (left && !right) {
      setMotorSpeeds(TURN_SPEED, DRIVE_SPEED);
    } else if (right && !left) {
      setMotorSpeeds(DRIVE_SPEED, TURN_SPEED);
    } else {
      setMotorSpeeds(DRIVE_SPEED, DRIVE_SPEED);
    }

    return;
  }

  // Backward modes
  if (backward) {
    if (left && !right) {
      setMotorSpeeds(-TURN_SPEED, -DRIVE_SPEED);
    } else if (right && !left) {
      setMotorSpeeds(-DRIVE_SPEED, -TURN_SPEED);
    } else {
      setMotorSpeeds(-DRIVE_SPEED, -DRIVE_SPEED);
    }

    return;
  }

  // Spin in place
  if (left && !right) {
    setMotorSpeeds(-DRIVE_SPEED, DRIVE_SPEED);
    return;
  }

  if (right && !left) {
    setMotorSpeeds(DRIVE_SPEED, -DRIVE_SPEED);
    return;
  }

  // Left + right with no throttle: stop
  stopMotors();
}

// --------------------------------------------------
// Motor control
// --------------------------------------------------
void setMotorSpeeds(int leftSpeed, int rightSpeed) {
  setSingleMotor(IN1, IN2, ENA, leftSpeed);
  setSingleMotor(IN3, IN4, ENB, rightSpeed);
}

void setSingleMotor(byte pinA, byte pinB, byte enablePin, int signedSpeed) {
  int pwm = abs(signedSpeed);

  if (pwm > 255) {
    pwm = 255;
  }

  analogWrite(enablePin, pwm);

  if (signedSpeed > 0) {
    digitalWrite(pinA, HIGH);
    digitalWrite(pinB, LOW);
  } else if (signedSpeed < 0) {
    digitalWrite(pinA, LOW);
    digitalWrite(pinB, HIGH);
  } else {
    digitalWrite(pinA, LOW);
    digitalWrite(pinB, LOW);
  }
}

void stopMotors() {
  analogWrite(ENA, 0);
  analogWrite(ENB, 0);

  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

// --------------------------------------------------
// Helpers
// --------------------------------------------------
void resetMotionTimer() {
  motionActive = false;
  motionStartTime = 0;
}

bool startsWith(const char *text, const char *prefix) {
  while (*prefix != '\0') {
    if (*text == '\0') {
      return false;
    }

    if (*text != *prefix) {
      return false;
    }

    text++;
    prefix++;
  }

  return true;
}
