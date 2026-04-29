#include <SoftwareSerial.h>

// HC-05 wiring:
// Arduino pin 10 = RX  <- HC-05 TXD
// Arduino pin 11 = TX  -> HC-05 RXD
SoftwareSerial BT(10, 11);

// L298N control pins
// ENA and ENB are NOT connected to Arduino.
// Their jumper caps stay installed on the L298N board.
const byte IN1 = 2;  // Left motor
const byte IN2 = 3;  // Left motor
const byte IN3 = 4;  // Right motor
const byte IN4 = 5;  // Right motor

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

// Serial line buffer
char inputBuffer[24];
byte inputIndex = 0;

// Current control state
byte currentMask = 0;

// Safety / timing state
bool motionActive = false;
bool safetyLocked = false;
unsigned long motionStartTime = 0;
unsigned long lastCommandTime = 0;

enum DriveMode {
  MODE_STOP,
  MODE_FORWARD,
  MODE_BACKWARD,
  MODE_FORWARD_LEFT,
  MODE_FORWARD_RIGHT,
  MODE_BACKWARD_LEFT,
  MODE_BACKWARD_RIGHT,
  MODE_SPIN_LEFT,
  MODE_SPIN_RIGHT
};

// --------------------------------------------------
// Function declarations for ArduinoDroid compatibility
// --------------------------------------------------
void readBluetoothLines();
void handleLine(const char *line);
void updateDriveControl();

DriveMode getDriveModeFromMask(byte mask);
void applyDriveMode(DriveMode mode);

void leftForward();
void leftBackward();
void leftStop();

void rightForward();
void rightBackward();
void rightStop();

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

    stopMotors();
    resetMotionTimer();
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

  DriveMode mode = getDriveModeFromMask(currentMask);
  applyDriveMode(mode);
}

DriveMode getDriveModeFromMask(byte mask) {
  bool forward  = (mask & BIT_FORWARD)  != 0;
  bool backward = (mask & BIT_BACKWARD) != 0;
  bool left     = (mask & BIT_LEFT)     != 0;
  bool right    = (mask & BIT_RIGHT)    != 0;

  // Forward + Backward together is unsafe/conflicting
  if (forward && backward) {
    return MODE_STOP;
  }

  // Forward combinations
  if (forward) {
    if (left && !right) {
      return MODE_FORWARD_LEFT;
    }

    if (right && !left) {
      return MODE_FORWARD_RIGHT;
    }

    return MODE_FORWARD;
  }

  // Backward combinations
  if (backward) {
    if (left && !right) {
      return MODE_BACKWARD_LEFT;
    }

    if (right && !left) {
      return MODE_BACKWARD_RIGHT;
    }

    return MODE_BACKWARD;
  }

  // Left / Right only: spin in place
  if (left && !right) {
    return MODE_SPIN_LEFT;
  }

  if (right && !left) {
    return MODE_SPIN_RIGHT;
  }

  // Left + Right together with no forward/backward
  return MODE_STOP;
}

void applyDriveMode(DriveMode mode) {
  switch (mode) {
    case MODE_FORWARD:
      leftForward();
      rightForward();
      break;

    case MODE_BACKWARD:
      leftBackward();
      rightBackward();
      break;

    case MODE_FORWARD_LEFT:
      leftStop();
      rightForward();
      break;

    case MODE_FORWARD_RIGHT:
      leftForward();
      rightStop();
      break;

    case MODE_BACKWARD_LEFT:
      leftStop();
      rightBackward();
      break;

    case MODE_BACKWARD_RIGHT:
      leftBackward();
      rightStop();
      break;

    case MODE_SPIN_LEFT:
      leftBackward();
      rightForward();
      break;

    case MODE_SPIN_RIGHT:
      leftForward();
      rightBackward();
      break;

    case MODE_STOP:
    default:
      stopMotors();
      break;
  }
}

// --------------------------------------------------
// Left motor helpers
// --------------------------------------------------
void leftForward() {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
}

void leftBackward() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
}

void leftStop() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
}

// --------------------------------------------------
// Right motor helpers
// --------------------------------------------------
void rightForward() {
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void rightBackward() {
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

void rightStop() {
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

// --------------------------------------------------
// General helpers
// --------------------------------------------------
void stopMotors() {
  leftStop();
  rightStop();
}

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
