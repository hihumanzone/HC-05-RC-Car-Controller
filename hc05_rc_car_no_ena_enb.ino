#include <SoftwareSerial.h>

SoftwareSerial BT(10, 11);  // HC-05 TX -> Arduino 10, HC-05 RX -> Arduino 11

// L298N direction pins only
const byte IN1 = 2;
const byte IN2 = 3;
const byte IN3 = 4;
const byte IN4 = 5;

// Safety timeout
const unsigned long SAFETY_TIMEOUT_MS = 3000;

// Optional debug output
const bool DEBUG_SERIAL = false;

// Button states from Bluetooth
bool forwardPressed  = false;
bool backwardPressed = false;
bool leftPressed     = false;
bool rightPressed    = false;

// Motion / safety state
bool motionActive = false;
bool safetyLocked = false;
unsigned long motionStartTime = 0;

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

enum MotorDirection {
  MOTOR_STOP,
  MOTOR_FORWARD,
  MOTOR_BACKWARD
};

void setup() {
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  Serial.begin(9600);
  BT.begin(9600);

  stopMotors();
}

void loop() {
  readBluetoothCommands();
  updateDriveControl();
}

void readBluetoothCommands() {
  while (BT.available() > 0) {
    char command = BT.read();

    if (DEBUG_SERIAL) {
      Serial.print("Received: ");
      Serial.println(command);
    }

    handleBluetoothCommand(command);
  }
}

void handleBluetoothCommand(char command) {
  switch (command) {
    // Button pressed
    case 'F': forwardPressed  = true;  break;
    case 'B': backwardPressed = true;  break;
    case 'L': leftPressed     = true;  break;
    case 'R': rightPressed    = true;  break;

    // Button released
    case 'f': forwardPressed  = false; break;
    case 'b': backwardPressed = false; break;
    case 'l': leftPressed     = false; break;
    case 'r': rightPressed    = false; break;

    // Optional extra stop commands
    case 'S':
    case 's':
    case 'T':
    case 't':
      clearAllButtons();
      break;

    default:
      // Ignore unknown characters
      break;
  }
}

void updateDriveControl() {
  // Release of all buttons resets everything
  if (!anyButtonPressed()) {
    stopMotors();
    resetMotionState();
    return;
  }

  // After safety timeout, remain stopped until all buttons are released
  if (safetyLocked) {
    stopMotors();
    return;
  }

  DriveMode mode = getRequestedDriveMode();

  // Conflicting or unclear input -> stop safely
  if (mode == MODE_STOP) {
    stopMotors();
    motionActive = false;
    return;
  }

  // Start safety timer only when actual motion starts
  if (!motionActive) {
    motionActive = true;
    motionStartTime = millis();
  }

  // Enforce 3-second safety auto-stop
  if (millis() - motionStartTime >= SAFETY_TIMEOUT_MS) {
    stopMotors();
    motionActive = false;
    safetyLocked = true;

    if (DEBUG_SERIAL) {
      Serial.println("Safety timeout triggered");
    }
    return;
  }

  applyDriveMode(mode);
}

DriveMode getRequestedDriveMode() {
  // Conflicting forward/backward -> stop
  if (forwardPressed && backwardPressed) {
    return MODE_STOP;
  }

  // Forward combinations
  if (forwardPressed) {
    if (leftPressed && !rightPressed)  return MODE_FORWARD_LEFT;
    if (rightPressed && !leftPressed)  return MODE_FORWARD_RIGHT;
    if (!leftPressed && !rightPressed) return MODE_FORWARD;
    return MODE_FORWARD; // left + right together cancel steering
  }

  // Backward combinations
  if (backwardPressed) {
    if (leftPressed && !rightPressed)  return MODE_BACKWARD_LEFT;
    if (rightPressed && !leftPressed)  return MODE_BACKWARD_RIGHT;
    if (!leftPressed && !rightPressed) return MODE_BACKWARD;
    return MODE_BACKWARD; // left + right together cancel steering
  }

  // Spin in place only when no forward/backward is pressed
  if (leftPressed && !rightPressed)  return MODE_SPIN_LEFT;
  if (rightPressed && !leftPressed)  return MODE_SPIN_RIGHT;

  // Left + right together with no drive direction -> stop
  return MODE_STOP;
}

void applyDriveMode(DriveMode mode) {
  switch (mode) {
    case MODE_FORWARD:
      setLeftMotor(MOTOR_FORWARD);
      setRightMotor(MOTOR_FORWARD);
      break;

    case MODE_BACKWARD:
      setLeftMotor(MOTOR_BACKWARD);
      setRightMotor(MOTOR_BACKWARD);
      break;

    case MODE_FORWARD_LEFT:
      // Simple turn without PWM:
      // left motor stopped, right motor forward
      setLeftMotor(MOTOR_STOP);
      setRightMotor(MOTOR_FORWARD);
      break;

    case MODE_FORWARD_RIGHT:
      // left motor forward, right motor stopped
      setLeftMotor(MOTOR_FORWARD);
      setRightMotor(MOTOR_STOP);
      break;

    case MODE_BACKWARD_LEFT:
      // left motor stopped, right motor backward
      setLeftMotor(MOTOR_STOP);
      setRightMotor(MOTOR_BACKWARD);
      break;

    case MODE_BACKWARD_RIGHT:
      // left motor backward, right motor stopped
      setLeftMotor(MOTOR_BACKWARD);
      setRightMotor(MOTOR_STOP);
      break;

    case MODE_SPIN_LEFT:
      setLeftMotor(MOTOR_BACKWARD);
      setRightMotor(MOTOR_FORWARD);
      break;

    case MODE_SPIN_RIGHT:
      setLeftMotor(MOTOR_FORWARD);
      setRightMotor(MOTOR_BACKWARD);
      break;

    case MODE_STOP:
    default:
      stopMotors();
      break;
  }
}

void setLeftMotor(MotorDirection direction) {
  setMotor(IN1, IN2, direction);
}

void setRightMotor(MotorDirection direction) {
  setMotor(IN3, IN4, direction);
}

void setMotor(byte pinA, byte pinB, MotorDirection direction) {
  switch (direction) {
    case MOTOR_FORWARD:
      digitalWrite(pinA, HIGH);
      digitalWrite(pinB, LOW);
      break;

    case MOTOR_BACKWARD:
      digitalWrite(pinA, LOW);
      digitalWrite(pinB, HIGH);
      break;

    case MOTOR_STOP:
    default:
      digitalWrite(pinA, LOW);
      digitalWrite(pinB, LOW);
      break;
  }
}

void stopMotors() {
  setLeftMotor(MOTOR_STOP);
  setRightMotor(MOTOR_STOP);
}

bool anyButtonPressed() {
  return forwardPressed || backwardPressed || leftPressed || rightPressed;
}

void clearAllButtons() {
  forwardPressed  = false;
  backwardPressed = false;
  leftPressed     = false;
  rightPressed    = false;
}

void resetMotionState() {
  motionActive = false;
  safetyLocked = false;
  motionStartTime = 0;
}
