#include <SoftwareSerial.h>

// HC-05:
// Arduino pin 10 = SoftwareSerial RX <- HC-05 TXD
// Arduino pin 11 = SoftwareSerial TX -> HC-05 RXD
SoftwareSerial BT(10, 11);

// L298N direction pins only
// ENA and ENB are NOT connected to Arduino in this build.
// Leave the ENA and ENB jumper caps installed on the L298N.
const byte IN1 = 2;  // Left motor direction A
const byte IN2 = 3;  // Left motor direction B
const byte IN3 = 4;  // Right motor direction A
const byte IN4 = 5;  // Right motor direction B

// Web app command bits: STATE:<number>
const byte BIT_FORWARD  = 1;
const byte BIT_BACKWARD = 2;
const byte BIT_LEFT     = 4;
const byte BIT_RIGHT    = 8;

const unsigned long SAFETY_TIMEOUT_MS = 3000;
const unsigned long SIGNAL_TIMEOUT_MS = 400;
const bool DEBUG_SERIAL = false;

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

byte currentMask = 0;
bool safetyLocked = false;
bool motionStarted = false;
unsigned long motionStartTime = 0;
unsigned long lastCommandTime = 0;

char lineBuffer[24];
byte lineLength = 0;

void setup() {
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  stopMotors();

  Serial.begin(9600);
  BT.begin(9600);

  lastCommandTime = millis();
  sendStatus("READY");
}

void loop() {
  readBluetoothMessages();
  updateSafetyState();
  applyCurrentDriveMode();
}

void readBluetoothMessages() {
  while (BT.available() > 0) {
    char c = (char)BT.read();

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      lineBuffer[lineLength] = '\0';
      processLine(lineBuffer);
      lineLength = 0;
      continue;
    }

    if (lineLength < sizeof(lineBuffer) - 1) {
      lineBuffer[lineLength++] = c;
    } else {
      // Reset if an invalid or too-long line arrives.
      lineLength = 0;
    }
  }
}

void processLine(const char* line) {
  if (line[0] == '\0') {
    return;
  }

  if (DEBUG_SERIAL) {
    Serial.print("RX: ");
    Serial.println(line);
  }

  if (strncmp(line, "STATE:", 6) != 0) {
    return;
  }

  int maskValue = atoi(line + 6);
  if (maskValue < 0) {
    maskValue = 0;
  }
  if (maskValue > 15) {
    maskValue = 15;
  }

  currentMask = (byte)maskValue;
  lastCommandTime = millis();

  if (currentMask == 0) {
    resetSafetyLock();
  }

  if (DEBUG_SERIAL) {
    Serial.print("Mask set to: ");
    Serial.println(currentMask);
  }
}

void updateSafetyState() {
  unsigned long now = millis();

  // If commands stop arriving while the car is moving, stop the car.
  if (currentMask != 0 && now - lastCommandTime >= SIGNAL_TIMEOUT_MS) {
    currentMask = 0;
    resetSafetyLock();
    stopMotors();
    sendStatus("SIGNAL_LOST_STOP");
    return;
  }

  // When all buttons are released, reset the motion timer.
  if (currentMask == 0) {
    motionStarted = false;
    return;
  }

  // Stay stopped after the 3-second cutoff until all buttons are released.
  if (safetyLocked) {
    return;
  }

  if (!motionStarted) {
    motionStarted = true;
    motionStartTime = now;
  }

  if (now - motionStartTime >= SAFETY_TIMEOUT_MS) {
    currentMask = 0;
    motionStarted = false;
    safetyLocked = true;
    stopMotors();
    sendStatus("SAFETY_LOCK");
  }
}

void applyCurrentDriveMode() {
  if (safetyLocked || currentMask == 0) {
    stopMotors();
    return;
  }

  DriveMode mode = getDriveModeFromMask(currentMask);
  applyDriveMode(mode);
}

DriveMode getDriveModeFromMask(byte mask) {
  bool forwardPressed = (mask & BIT_FORWARD) != 0;
  bool backwardPressed = (mask & BIT_BACKWARD) != 0;
  bool leftPressed = (mask & BIT_LEFT) != 0;
  bool rightPressed = (mask & BIT_RIGHT) != 0;

  // Conflicting commands -> stop safely.
  if (forwardPressed && backwardPressed) {
    return MODE_STOP;
  }

  if (!forwardPressed && !backwardPressed && leftPressed && rightPressed) {
    return MODE_STOP;
  }

  if (forwardPressed) {
    if (leftPressed && !rightPressed) {
      return MODE_FORWARD_LEFT;
    }
    if (rightPressed && !leftPressed) {
      return MODE_FORWARD_RIGHT;
    }
    return MODE_FORWARD;
  }

  if (backwardPressed) {
    if (leftPressed && !rightPressed) {
      return MODE_BACKWARD_LEFT;
    }
    if (rightPressed && !leftPressed) {
      return MODE_BACKWARD_RIGHT;
    }
    return MODE_BACKWARD;
  }

  if (leftPressed && !rightPressed) {
    return MODE_SPIN_LEFT;
  }

  if (rightPressed && !leftPressed) {
    return MODE_SPIN_RIGHT;
  }

  return MODE_STOP;
}

void applyDriveMode(DriveMode mode) {
  switch (mode) {
    case MODE_FORWARD:
      leftMotorForward();
      rightMotorForward();
      break;

    case MODE_BACKWARD:
      leftMotorBackward();
      rightMotorBackward();
      break;

    case MODE_FORWARD_LEFT:
      // No PWM available, so turn by stopping the inner wheel.
      leftMotorStop();
      rightMotorForward();
      break;

    case MODE_FORWARD_RIGHT:
      leftMotorForward();
      rightMotorStop();
      break;

    case MODE_BACKWARD_LEFT:
      leftMotorStop();
      rightMotorBackward();
      break;

    case MODE_BACKWARD_RIGHT:
      leftMotorBackward();
      rightMotorStop();
      break;

    case MODE_SPIN_LEFT:
      leftMotorBackward();
      rightMotorForward();
      break;

    case MODE_SPIN_RIGHT:
      leftMotorForward();
      rightMotorBackward();
      break;

    case MODE_STOP:
    default:
      stopMotors();
      break;
  }
}

void leftMotorForward() {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
}

void leftMotorBackward() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
}

void leftMotorStop() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
}

void rightMotorForward() {
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void rightMotorBackward() {
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

void rightMotorStop() {
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void stopMotors() {
  leftMotorStop();
  rightMotorStop();
}

void resetSafetyLock() {
  bool wasLocked = safetyLocked;
  safetyLocked = false;
  motionStarted = false;
  motionStartTime = 0;

  if (wasLocked) {
    sendStatus("UNLOCKED");
  }
}

void sendStatus(const char* message) {
  BT.println(message);

  if (DEBUG_SERIAL) {
    Serial.println(message);
  }
}
