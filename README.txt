HC-05 RC Car PWA
================

Files
-----
- index.html              Main controller page
- styles.css              UI styling
- app.js                  PWA + Bluetooth serial logic
- manifest.webmanifest    PWA manifest
- sw.js                   Service worker for offline installable shell
- icons/                  App icons
- hc05_rc_car.ino         Arduino sketch for L298N ENA/ENB connected to pins 6/9
- hc05_rc_car_no_ena_enb.ino
                          Arduino sketch for L298N ENA/ENB jumpers installed
- README.txt              Project instructions

Hardware
--------
- Arduino Uno
- HC-05 Bluetooth module
- L298N motor driver module
- 2 DC motors
- 2-wheel robot chassis
- Battery pack
- Jumper wires
- Wheels
- Caster wheel
- 2 HC-SR04-compatible ultrasonic sensors, recommended as front and rear sensors

Wiring
------

HC-05 to Arduino Uno:
- HC-05 VCC  -> Arduino 5V
- HC-05 GND  -> Arduino GND
- HC-05 TXD  -> Arduino pin 10
- HC-05 RXD  -> Arduino pin 11

L298N to Arduino Uno:
- L298N IN1  -> Arduino pin 2
- L298N IN2  -> Arduino pin 3
- L298N IN3  -> Arduino pin 4
- L298N IN4  -> Arduino pin 5
- L298N GND  -> Arduino GND

L298N enable pins, jumper version:
- ENA -> Jumper installed, not connected to Arduino
- ENB -> Jumper installed, not connected to Arduino
- Use hc05_rc_car_no_ena_enb.ino

L298N enable pins, PWM version:
- ENA -> Arduino pin 6
- ENB -> Arduino pin 9
- Remove the ENA and ENB jumper caps from the L298N
- Use hc05_rc_car.ino

Ultrasonic sensors to Arduino Uno:
- Front sensor VCC  -> Arduino 5V
- Front sensor GND  -> Arduino GND
- Front sensor TRIG -> Arduino A0
- Front sensor ECHO -> Arduino A1
- Rear sensor VCC   -> Arduino 5V
- Rear sensor GND   -> Arduino GND
- Rear sensor TRIG  -> Arduino A2
- Rear sensor ECHO  -> Arduino A3

Notes for ultrasonic wiring:
- A0 to A3 are used as digital pins.
- Keep the front and rear sensors aimed away from each other to reduce echo crosstalk.
- If you use a 3.3V microcontroller instead of an Arduino Uno, level-shift the ECHO pins.

Motors to L298N:
- Left motor  -> OUT1 and OUT2
- Right motor -> OUT3 and OUT4

Battery to L298N:
- Battery + -> L298N 12V / VIN
- Battery - -> L298N GND

Common ground:
- Arduino GND
- HC-05 GND
- L298N GND
- Battery -
- Front ultrasonic GND
- Rear ultrasonic GND

All grounds must be connected together.

How to use
----------
1. Choose the sketch that matches your L298N wiring:
   - hc05_rc_car_no_ena_enb.ino if ENA and ENB jumpers stay installed.
   - hc05_rc_car.ino if ENA and ENB are connected to Arduino pins 6 and 9.
2. Upload the chosen sketch to your Arduino Uno.
3. Make the wiring shown above, including the ultrasonic sensors on A0 to A3.
4. Pair the HC-05 in Android Bluetooth settings.
5. Serve this folder over HTTPS.

   Examples:
   - GitHub Pages
   - Netlify
   - Any HTTPS web host

6. Open index.html in Chrome on Android.
7. Tap Connect and choose the HC-05 serial device.
8. Install the PWA with the Install App button or Chrome's Add to Home Screen option.

Command format
--------------
The web app sends lines like:

STATE:0
STATE:1
STATE:5

Bit mask meaning:

1 = Forward
2 = Backward
4 = Left
8 = Right

Examples:

- STATE:1  = Forward
- STATE:2  = Backward
- STATE:4  = Left
- STATE:8  = Right
- STATE:5  = Forward + Left
- STATE:9  = Forward + Right
- STATE:6  = Backward + Left
- STATE:10 = Backward + Right
- STATE:0  = Stop

Extra command:

- SENSOR? = Arduino immediately reports the latest ultrasonic readings and obstacle state.

Arduino telemetry
-----------------
The Arduino now sends ultrasonic status lines over Bluetooth:

- READY
- SENSORS:ULTRASONIC_READY
- DIST:F=42,R=88
- DIST:F=--,R=31
- OBSTACLE:FRONT
- OBSTACLE:REAR
- OBSTACLE:BOTH
- OBSTACLE:CLEAR
- SAFETY_LOCK
- UNLOCKED

Distance values are in centimeters. A value of -- means no valid reading is currently available.

Ultrasonic behavior
-------------------
- The front sensor blocks forward movement when an object is about 20 cm or closer.
- The rear sensor blocks backward movement when an object is about 20 cm or closer.
- Movement is allowed again after the distance rises above about 28 cm. This hysteresis prevents jitter near the threshold.
- Sensor values are filtered in code to reduce HC-SR04 noise.
- The code reads one sensor at a time to reduce crosstalk and keep Bluetooth commands responsive.
- The web controller displays live front and rear distances.
- If an obstacle is detected, the web controller clears held buttons, sends a stop command, and shows an obstacle warning.
- Spin/turn-only commands remain available so the car can be steered away from an obstacle.

Behavior
--------
- Press and hold a button to move.
- Release the button to stop.
- The Arduino stops immediately when it receives STATE:0.
- The Arduino stops automatically after 3 seconds as a safety limit.
- After the 3-second safety stop, release all buttons before moving again.
- The web app repeats the current STATE while buttons are held so control stays reliable.
- Ultrasonic obstacle stops are separate from the 3-second safety stop.

Tuning
------
Open the Arduino sketch and adjust these constants if needed:

- OBSTACLE_STOP_CM: distance at or below which movement is blocked. Default: 20.
- OBSTACLE_CLEAR_CM: distance at which the obstacle is considered clear. Default: 28.
- ULTRASONIC_MAX_CM: maximum distance to measure. Default: 200.
- TELEMETRY_INTERVAL_MS: how often distance data is sent to the web app. Default: 500.

Notes
-----
- If a motor spins in the wrong direction, swap that motor's two wires on the L298N output terminals.
- The service worker lets the controller UI load like an installed app.
- Bluetooth still requires a compatible browser and device.
- Web Serial over Bluetooth works best in Chrome on Android after the HC-05 has already been paired in system Bluetooth settings.
