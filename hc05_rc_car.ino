#include <SoftwareSerial.h>

// HC-05 wiring:
// Arduino pin 10 = RX  <- HC-05 TXD
// Arduino pin 11 = TX  -> HC-05 RXD
SoftwareSerial BT(10, 11);

// L298N pins. Use this sketch when ENA and ENB are wired to Arduino PWM pins.
const byte IN1 = 2;
const byte IN2 = 3;
const byte IN3 = 4;
const byte IN4 = 5;
const byte ENA = 6;
const byte ENB = 9;

// Front HC-SR04 / compatible ultrasonic sensor pins.
// A0 and A1 are used as digital pins so the motor, PWM, and HC-05 pins stay free.
const byte FRONT_TRIG_PIN = A0;
const byte FRONT_ECHO_PIN = A1;

// Speed settings
const byte DRIVE_SPEED = 200;
const byte TURN_SPEED = 120;

// Safety settings
const unsigned long SAFETY_TIMEOUT_MS = 3000;
const unsigned long COMMAND_LOSS_TIMEOUT_MS = 350;

// Front ultrasonic settings
const bool FRONT_ULTRASONIC_ENABLED = true;
const unsigned int OBSTACLE_STOP_CM = 20;
const unsigned int OBSTACLE_CLEAR_CM = 28;
const unsigned int DISTANCE_INVALID_CM = 999;
const unsigned int ULTRASONIC_MAX_CM = 200;
const unsigned long ULTRASONIC_TIMEOUT_US = ULTRASONIC_MAX_CM * 58UL;
const unsigned long ULTRASONIC_READ_INTERVAL_MS = 70;
const unsigned long DISTANCE_STALE_MS = 1000;
const unsigned long TELEMETRY_INTERVAL_MS = 500;

const char FIRMWARE_VERSION[] = "FRONT_ULTRASONIC_FINAL_1";
const bool DEBUG_SERIAL = false;

// Incoming command format from web app:
// STATE:<mask>
//
// Bit mask:
// 1 = Forward
// 2 = Backward
// 4 = Left
// 8 = Right
//
// Extra commands:
// SENSOR? = immediately reports the latest front ultrasonic reading and obstacle state
// VERSION? = reports the firmware version
const byte BIT_FORWARD  = 1;
const byte BIT_BACKWARD = 2;
const byte BIT_LEFT     = 4;
const byte BIT_RIGHT    = 8;

struct UltrasonicSensorState {
  byte triggerPin;
  byte echoPin;
  unsigned int distanceCm;
  unsigned long lastValidMs;
  unsigned long lastReadMs;
  unsigned long lastTelemetryMs;
  bool obstacle;
  bool lastReportedObstacle;
};

UltrasonicSensorState frontSensor = {
  FRONT_TRIG_PIN,
  FRONT_ECHO_PIN,
  DISTANCE_INVALID_CM,
  0,
  0,
  0,
  false,
  false
};

char inputBuffer[24];
byte inputIndex = 0;
byte currentMask = 0;

bool motionActive = false;
bool safetyLocked = false;
bool forwardReleaseRequiredAfterObstacle = false;
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
void setMotorSpeeds(int leftSpeed, int rightSpeed);
void setSingleMotor(byte pinA, byte pinB, byte enablePin, int signedSpeed);
void stopMotors();
void resetMotionTimer();

void setupFrontUltrasonicSensor();
void serviceFrontUltrasonicSensor(bool forceRead);
unsigned int readUltrasonicCm(byte triggerPin, byte echoPin);
void storeFrontDistanceReading(unsigned int rawDistance);
bool calculateObstacleState(bool wasObstacle, unsigned int distanceCm);
bool commandIncludesForwardDrive(byte mask);
bool frontSensorBlocksCommand(byte mask);
void sendFrontDistanceTelemetry(bool forceSend);
void sendFrontObstacleTelemetry(bool forceSend);
void sendFirmwareVersion();
void printDistanceValue(unsigned int distanceCm);

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

  setupFrontUltrasonicSensor();

  Serial.begin(9600);
  BT.begin(9600);

  stopMotors();
  serviceFrontUltrasonicSensor(true);

  BT.println("READY");
  sendFirmwareVersion();
  if (FRONT_ULTRASONIC_ENABLED) {
    BT.println("SENSORS:FRONT_ULTRASONIC_READY");
    sendFrontDistanceTelemetry(true);
    sendFrontObstacleTelemetry(true);
  }

  if (DEBUG_SERIAL) {
    Serial.println("READY");
    Serial.print("VERSION:");
    Serial.println(FIRMWARE_VERSION);
  }
}

// --------------------------------------------------
// Main loop
// --------------------------------------------------
void loop() {
  readBluetoothLines();
  serviceFrontUltrasonicSensor(false);
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
      // Buffer overflow protection: drop the partial command.
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
    serviceFrontUltrasonicSensor(true);
    sendFrontDistanceTelemetry(true);
    sendFrontObstacleTelemetry(true);
    return;
  }

  if (startsWith(line, "VERSION?")) {
    sendFirmwareVersion();
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

  if (!commandIncludesForwardDrive(currentMask)) {
    forwardReleaseRequiredAfterObstacle = false;
  }

  // STATE:0 means all buttons released.
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
  if (currentMask == 0) {
    stopMotors();
    resetMotionTimer();
    return;
  }

  if (safetyLocked) {
    stopMotors();
    return;
  }

  if (millis() - lastCommandTime > COMMAND_LOSS_TIMEOUT_MS) {
    currentMask = 0;
    forwardReleaseRequiredAfterObstacle = false;
    stopMotors();
    resetMotionTimer();

    if (DEBUG_SERIAL) {
      Serial.println("COMMAND TIMEOUT");
    }

    return;
  }

  // Front obstacle protection blocks only forward drive commands. Reverse and
  // spin/turn-only commands remain available so the car can move away safely.
  if (frontSensorBlocksCommand(currentMask)) {
    stopMotors();
    resetMotionTimer();
    return;
  }

  if (!motionActive) {
    motionActive = true;
    motionStartTime = millis();
  }

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

  applyDriveMode(getDriveModeFromMask(currentMask));
}

DriveMode getDriveModeFromMask(byte mask) {
  bool forward  = (mask & BIT_FORWARD)  != 0;
  bool backward = (mask & BIT_BACKWARD) != 0;
  bool left     = (mask & BIT_LEFT)     != 0;
  bool right    = (mask & BIT_RIGHT)    != 0;

  if (forward && backward) {
    return MODE_STOP;
  }

  if (forward) {
    if (left && !right) {
      return MODE_FORWARD_LEFT;
    }
    if (right && !left) {
      return MODE_FORWARD_RIGHT;
    }
    return MODE_FORWARD;
  }

  if (backward) {
    if (left && !right) {
      return MODE_BACKWARD_LEFT;
    }
    if (right && !left) {
      return MODE_BACKWARD_RIGHT;
    }
    return MODE_BACKWARD;
  }

  if (left && !right) {
    return MODE_SPIN_LEFT;
  }

  if (right && !left) {
    return MODE_SPIN_RIGHT;
  }

  return MODE_STOP;
}

void applyDriveMode(DriveMode mode) {
  switch (mode) {
    case MODE_FORWARD:
      setMotorSpeeds(DRIVE_SPEED, DRIVE_SPEED);
      break;

    case MODE_BACKWARD:
      setMotorSpeeds(-DRIVE_SPEED, -DRIVE_SPEED);
      break;

    case MODE_FORWARD_LEFT:
      setMotorSpeeds(TURN_SPEED, DRIVE_SPEED);
      break;

    case MODE_FORWARD_RIGHT:
      setMotorSpeeds(DRIVE_SPEED, TURN_SPEED);
      break;

    case MODE_BACKWARD_LEFT:
      setMotorSpeeds(-TURN_SPEED, -DRIVE_SPEED);
      break;

    case MODE_BACKWARD_RIGHT:
      setMotorSpeeds(-DRIVE_SPEED, -TURN_SPEED);
      break;

    case MODE_SPIN_LEFT:
      setMotorSpeeds(-DRIVE_SPEED, DRIVE_SPEED);
      break;

    case MODE_SPIN_RIGHT:
      setMotorSpeeds(DRIVE_SPEED, -DRIVE_SPEED);
      break;

    case MODE_STOP:
    default:
      stopMotors();
      break;
  }
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

void resetMotionTimer() {
  motionActive = false;
  motionStartTime = 0;
}

// --------------------------------------------------
// Front ultrasonic sensor
// --------------------------------------------------
void setupFrontUltrasonicSensor() {
  if (!FRONT_ULTRASONIC_ENABLED) {
    return;
  }

  pinMode(frontSensor.triggerPin, OUTPUT);
  pinMode(frontSensor.echoPin, INPUT);
  digitalWrite(frontSensor.triggerPin, LOW);
}

void serviceFrontUltrasonicSensor(bool forceRead) {
  if (!FRONT_ULTRASONIC_ENABLED) {
    return;
  }

  unsigned long now = millis();
  if (!forceRead && now - frontSensor.lastReadMs < ULTRASONIC_READ_INTERVAL_MS) {
    return;
  }

  frontSensor.lastReadMs = now;
  unsigned int rawDistance = readUltrasonicCm(frontSensor.triggerPin, frontSensor.echoPin);
  storeFrontDistanceReading(rawDistance);

  bool previousObstacleState = frontSensor.obstacle;
  frontSensor.obstacle = calculateObstacleState(frontSensor.obstacle, frontSensor.distanceCm);

  if (frontSensor.obstacle != previousObstacleState) {
    sendFrontObstacleTelemetry(false);
  }

  sendFrontDistanceTelemetry(false);
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

void storeFrontDistanceReading(unsigned int rawDistance) {
  unsigned long now = millis();

  if (rawDistance == DISTANCE_INVALID_CM) {
    if (frontSensor.lastValidMs == 0 || now - frontSensor.lastValidMs > DISTANCE_STALE_MS) {
      frontSensor.distanceCm = DISTANCE_INVALID_CM;
    }
    return;
  }

  if (frontSensor.distanceCm == DISTANCE_INVALID_CM) {
    frontSensor.distanceCm = rawDistance;
  } else {
    // Small low-pass filter to reduce HC-SR04 jitter without hiding close objects.
    frontSensor.distanceCm = (frontSensor.distanceCm * 2 + rawDistance) / 3;
  }

  frontSensor.lastValidMs = now;
}

bool calculateObstacleState(bool wasObstacle, unsigned int distanceCm) {
  if (distanceCm == DISTANCE_INVALID_CM) {
    return false;
  }

  if (wasObstacle) {
    return distanceCm < OBSTACLE_CLEAR_CM;
  }

  return distanceCm <= OBSTACLE_STOP_CM;
}

bool commandIncludesForwardDrive(byte mask) {
  bool forward  = (mask & BIT_FORWARD)  != 0;
  bool backward = (mask & BIT_BACKWARD) != 0;
  return forward && !backward;
}

bool frontSensorBlocksCommand(byte mask) {
  if (!commandIncludesForwardDrive(mask)) {
    forwardReleaseRequiredAfterObstacle = false;
    return false;
  }

  if (frontSensor.obstacle) {
    forwardReleaseRequiredAfterObstacle = true;
    return true;
  }

  // Once an obstacle has stopped a forward command, require the forward button
  // to be released before forward motion can resume. This prevents the car from
  // launching forward again the moment the reading clears.
  return forwardReleaseRequiredAfterObstacle;
}

void sendFrontDistanceTelemetry(bool forceSend) {
  if (!FRONT_ULTRASONIC_ENABLED) {
    return;
  }

  unsigned long now = millis();
  if (!forceSend && now - frontSensor.lastTelemetryMs < TELEMETRY_INTERVAL_MS) {
    return;
  }

  frontSensor.lastTelemetryMs = now;

  BT.print("DIST:F=");
  printDistanceValue(frontSensor.distanceCm);
  BT.println();

  if (DEBUG_SERIAL) {
    Serial.print("DIST:F=");
    if (frontSensor.distanceCm == DISTANCE_INVALID_CM) {
      Serial.print("--");
    } else {
      Serial.print(frontSensor.distanceCm);
    }
    Serial.println();
  }
}

void sendFrontObstacleTelemetry(bool forceSend) {
  if (!FRONT_ULTRASONIC_ENABLED) {
    return;
  }

  if (!forceSend && frontSensor.obstacle == frontSensor.lastReportedObstacle) {
    return;
  }

  if (frontSensor.obstacle) {
    BT.println("OBSTACLE:FRONT");
  } else {
    BT.println("OBSTACLE:CLEAR");
  }

  frontSensor.lastReportedObstacle = frontSensor.obstacle;
}

void sendFirmwareVersion() {
  BT.print("VERSION:");
  BT.println(FIRMWARE_VERSION);
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
