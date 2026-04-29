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

// Front HC-SR04 / ultrasonic sensor pins.
// Analog pins are used as digital pins so the motor, PWM, and HC-05 pins stay free.
const byte FRONT_TRIG_PIN = A0;
const byte FRONT_ECHO_PIN = A1;

// Speed settings
const byte DRIVE_SPEED = 200;
const byte TURN_SPEED  = 120;

// Safety settings
const unsigned long SAFETY_TIMEOUT_MS = 3000;
const unsigned long COMMAND_LOSS_TIMEOUT_MS = 350;

// Ultrasonic settings
const bool ULTRASONIC_ENABLED = true;
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
// SENSOR? = immediately prints the latest front ultrasonic telemetry
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

unsigned int frontDistanceCm = DISTANCE_INVALID_CM;
unsigned long lastFrontValidTime = 0;
unsigned long lastUltrasonicReadTime = 0;
unsigned long lastTelemetryTime = 0;
bool frontObstacle = false;
bool lastReportedFrontObstacle = false;

// --------------------------------------------------
// Function declarations for ArduinoDroid compatibility
// --------------------------------------------------
void readBluetoothLines();
void handleLine(const char *line);
void updateDriveControl();
void applyMask(byte mask);

void setMotorSpeeds(int leftSpeed, int rightSpeed);
void setSingleMotor(byte pinA, byte pinB, byte enablePin, int signedSpeed);

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
  pinMode(ENA, OUTPUT);
  pinMode(ENB, OUTPUT);

  if (ULTRASONIC_ENABLED) {
    pinMode(FRONT_TRIG_PIN, OUTPUT);
    pinMode(FRONT_ECHO_PIN, INPUT);
    digitalWrite(FRONT_TRIG_PIN, LOW);
  }

  Serial.begin(9600);
  BT.begin(9600);

  stopMotors();

  BT.println("READY");
  if (ULTRASONIC_ENABLED) {
    BT.println("SENSORS:FRONT_ULTRASONIC_READY");
    sendDistanceTelemetry(true);
  }

  if (DEBUG_SERIAL) {
    Serial.println("READY");
    if (ULTRASONIC_ENABLED) {
      Serial.println("SENSORS:FRONT_ULTRASONIC_READY");
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

  // Front ultrasonic obstacle lockout. Forward motion is blocked by the front
  // sensor. Reverse and spin/turn-only commands remain available so the car
  // can be backed or steered away from an obstacle.
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

  unsigned int rawFront = readUltrasonicCm(FRONT_TRIG_PIN, FRONT_ECHO_PIN);
  updateDistanceCm(frontDistanceCm, lastFrontValidTime, rawFront);

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

  frontObstacle = obstacleStateForDistance(frontObstacle, frontDistanceCm);

  if (frontObstacle != oldFrontObstacle) {
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
  BT.println();

  if (DEBUG_SERIAL) {
    Serial.print("DIST:F=");
    if (frontDistanceCm == DISTANCE_INVALID_CM) {
      Serial.print("--");
    } else {
      Serial.print(frontDistanceCm);
    }
    Serial.println();
  }
}

void sendObstacleEvents(bool forceSend) {
  if (!ULTRASONIC_ENABLED) {
    return;
  }

  if (!forceSend && frontObstacle == lastReportedFrontObstacle) {
    return;
  }

  if (frontObstacle) {
    BT.println("OBSTACLE:FRONT");
  } else {
    BT.println("OBSTACLE:CLEAR");
  }

  lastReportedFrontObstacle = frontObstacle;
}

void printDistanceValue(unsigned int distanceCm) {
  if (distanceCm == DISTANCE_INVALID_CM) {
    BT.print("--");
  } else {
    BT.print(distanceCm);
  }
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
