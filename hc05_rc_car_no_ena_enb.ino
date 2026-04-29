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

// HC-SR04 / ultrasonic sensor pins.
// Analog pins are used as digital pins so the motor and HC-05 pins stay free.
const byte FRONT_TRIG_PIN = A0;
const byte FRONT_ECHO_PIN = A1;
const byte REAR_TRIG_PIN  = A2;
const byte REAR_ECHO_PIN  = A3;

// Safety settings
const unsigned long SAFETY_TIMEOUT_MS = 3000;
const unsigned long COMMAND_LOSS_TIMEOUT_MS = 350;

// Ultrasonic settings
const bool ULTRASONIC_ENABLED = true;
const bool FRONT_ULTRASONIC_ENABLED = true;
const bool REAR_ULTRASONIC_ENABLED = true;
const unsigned int OBSTACLE_STOP_CM = 20;
const unsigned int OBSTACLE_CLEAR_CM = 28;
const unsigned int DISTANCE_INVALID_CM = 999;
const unsigned int ULTRASONIC_MAX_CM = 200;
const unsigned long ULTRASONIC_TIMEOUT_US = ULTRASONIC_MAX_CM * 58UL;
const unsigned long ULTRASONIC_READ_INTERVAL_MS = 70;
const unsigned long DISTANCE_STALE_MS = 1000;
const unsigned long TELEMETRY_INTERVAL_MS = 500;

// Incoming command format from web app:
// STATE:<mask>
//
// Bit mask:
// 1 = Forward
// 2 = Backward
// 4 = Left
// 8 = Right
//
// Extra command:
// SENSOR? = immediately prints the latest ultrasonic telemetry
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

// Ultrasonic state
unsigned int frontDistanceCm = DISTANCE_INVALID_CM;
unsigned int rearDistanceCm = DISTANCE_INVALID_CM;
unsigned long lastFrontValidTime = 0;
unsigned long lastRearValidTime = 0;
unsigned long lastUltrasonicReadTime = 0;
unsigned long lastTelemetryTime = 0;
bool readFrontNext = true;
bool frontObstacle = false;
bool rearObstacle = false;
bool lastReportedFrontObstacle = false;
bool lastReportedRearObstacle = false;

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

void updateUltrasonicSensors();
unsigned int readUltrasonicCm(byte triggerPin, byte echoPin);
void updateDistanceCm(unsigned int &storedDistance, unsigned long &lastValidTime, unsigned int rawDistance);
void updateObstacleFlags();
bool obstacleStateForDistance(bool wasObstacle, unsigned int distanceCm);
bool movementBlockedByObstacle(byte mask);
void sendDistanceTelemetry(bool forceSend);
void sendObstacleEvents(bool forceSend);
void printDistanceValue(unsigned int distanceCm);

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

  if (ULTRASONIC_ENABLED) {
    pinMode(FRONT_TRIG_PIN, OUTPUT);
    pinMode(FRONT_ECHO_PIN, INPUT);
    pinMode(REAR_TRIG_PIN, OUTPUT);
    pinMode(REAR_ECHO_PIN, INPUT);
    digitalWrite(FRONT_TRIG_PIN, LOW);
    digitalWrite(REAR_TRIG_PIN, LOW);
  }

  Serial.begin(9600);
  BT.begin(9600);

  stopMotors();

  BT.println("READY");
  if (ULTRASONIC_ENABLED) {
    BT.println("SENSORS:ULTRASONIC_READY");
    sendDistanceTelemetry(true);
  }

  if (DEBUG_SERIAL) {
    Serial.println("READY");
    if (ULTRASONIC_ENABLED) {
      Serial.println("SENSORS:ULTRASONIC_READY");
    }
  }
}

// --------------------------------------------------
// Main loop
// --------------------------------------------------
void loop() {
  readBluetoothLines();
  updateUltrasonicSensors();
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

  if (startsWith(line, "SENSOR?")) {
    sendDistanceTelemetry(true);
    sendObstacleEvents(true);
    return;
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

  // Ultrasonic obstacle lockout. Forward motion is blocked by the front sensor,
  // reverse motion is blocked by the rear sensor. Spin/turn-only commands remain
  // available so the car can be steered away from an obstacle.
  if (movementBlockedByObstacle(currentMask)) {
    stopMotors();
    resetMotionTimer();
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
// Ultrasonic sensors
// --------------------------------------------------
void updateUltrasonicSensors() {
  if (!ULTRASONIC_ENABLED) {
    return;
  }

  unsigned long now = millis();
  if (now - lastUltrasonicReadTime < ULTRASONIC_READ_INTERVAL_MS) {
    return;
  }
  lastUltrasonicReadTime = now;

  if (readFrontNext) {
    if (FRONT_ULTRASONIC_ENABLED) {
      unsigned int rawFront = readUltrasonicCm(FRONT_TRIG_PIN, FRONT_ECHO_PIN);
      updateDistanceCm(frontDistanceCm, lastFrontValidTime, rawFront);
    }
  } else {
    if (REAR_ULTRASONIC_ENABLED) {
      unsigned int rawRear = readUltrasonicCm(REAR_TRIG_PIN, REAR_ECHO_PIN);
      updateDistanceCm(rearDistanceCm, lastRearValidTime, rawRear);
    }
  }

  readFrontNext = !readFrontNext;
  updateObstacleFlags();
  sendDistanceTelemetry(false);
}

unsigned int readUltrasonicCm(byte triggerPin, byte echoPin) {
  digitalWrite(triggerPin, LOW);
  delayMicroseconds(2);
  digitalWrite(triggerPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(triggerPin, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, ULTRASONIC_TIMEOUT_US);

  if (duration == 0) {
    return DISTANCE_INVALID_CM;
  }

  unsigned int distanceCm = (unsigned int)(duration / 58UL);

  if (distanceCm == 0 || distanceCm > ULTRASONIC_MAX_CM) {
    return DISTANCE_INVALID_CM;
  }

  return distanceCm;
}

void updateDistanceCm(unsigned int &storedDistance, unsigned long &lastValidTime, unsigned int rawDistance) {
  unsigned long now = millis();

  if (rawDistance == DISTANCE_INVALID_CM) {
    if (lastValidTime == 0 || now - lastValidTime > DISTANCE_STALE_MS) {
      storedDistance = DISTANCE_INVALID_CM;
    }
    return;
  }

  if (storedDistance == DISTANCE_INVALID_CM) {
    storedDistance = rawDistance;
  } else {
    // Simple low-pass filter to reduce jitter from the HC-SR04 echo pulse.
    storedDistance = (storedDistance * 2 + rawDistance) / 3;
  }

  lastValidTime = now;
}

void updateObstacleFlags() {
  bool oldFrontObstacle = frontObstacle;
  bool oldRearObstacle = rearObstacle;

  frontObstacle = FRONT_ULTRASONIC_ENABLED && obstacleStateForDistance(frontObstacle, frontDistanceCm);
  rearObstacle = REAR_ULTRASONIC_ENABLED && obstacleStateForDistance(rearObstacle, rearDistanceCm);

  if (frontObstacle != oldFrontObstacle || rearObstacle != oldRearObstacle) {
    sendObstacleEvents(false);
  }
}

bool obstacleStateForDistance(bool wasObstacle, unsigned int distanceCm) {
  if (distanceCm == DISTANCE_INVALID_CM) {
    return false;
  }

  if (wasObstacle) {
    return distanceCm < OBSTACLE_CLEAR_CM;
  }

  return distanceCm <= OBSTACLE_STOP_CM;
}

bool movementBlockedByObstacle(byte mask) {
  bool forward  = (mask & BIT_FORWARD)  != 0;
  bool backward = (mask & BIT_BACKWARD) != 0;

  if (forward && !backward && frontObstacle) {
    return true;
  }

  if (backward && !forward && rearObstacle) {
    return true;
  }

  return false;
}

void sendDistanceTelemetry(bool forceSend) {
  if (!ULTRASONIC_ENABLED) {
    return;
  }

  unsigned long now = millis();
  if (!forceSend && now - lastTelemetryTime < TELEMETRY_INTERVAL_MS) {
    return;
  }
  lastTelemetryTime = now;

  BT.print("DIST:F=");
  printDistanceValue(frontDistanceCm);
  BT.print(",R=");
  printDistanceValue(rearDistanceCm);
  BT.println();

  if (DEBUG_SERIAL) {
    Serial.print("DIST:F=");
    if (frontDistanceCm == DISTANCE_INVALID_CM) {
      Serial.print("--");
    } else {
      Serial.print(frontDistanceCm);
    }
    Serial.print(",R=");
    if (rearDistanceCm == DISTANCE_INVALID_CM) {
      Serial.print("--");
    } else {
      Serial.print(rearDistanceCm);
    }
    Serial.println();
  }
}

void sendObstacleEvents(bool forceSend) {
  if (!ULTRASONIC_ENABLED) {
    return;
  }

  if (!forceSend && frontObstacle == lastReportedFrontObstacle && rearObstacle == lastReportedRearObstacle) {
    return;
  }

  if (frontObstacle && rearObstacle) {
    BT.println("OBSTACLE:BOTH");
  } else if (frontObstacle) {
    BT.println("OBSTACLE:FRONT");
  } else if (rearObstacle) {
    BT.println("OBSTACLE:REAR");
  } else {
    BT.println("OBSTACLE:CLEAR");
  }

  lastReportedFrontObstacle = frontObstacle;
  lastReportedRearObstacle = rearObstacle;
}

void printDistanceValue(unsigned int distanceCm) {
  if (distanceCm == DISTANCE_INVALID_CM) {
    BT.print("--");
  } else {
    BT.print(distanceCm);
  }
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
