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
- hc05_rc_car.ino         Matching Arduino sketch
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

L298N enable pins:
- ENA -> Jumper installed, not connected to Arduino
- ENB -> Jumper installed, not connected to Arduino

Arduino pins 6 and 9 are not used.

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

All grounds must be connected together.

How to use
----------
1. Upload hc05_rc_car_no_ena_enb.ino to your Arduino Uno.
2. Make the wiring shown above.
3. Keep the ENA and ENB jumpers installed on the L298N.
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

Behavior
--------
- Press and hold a button to move.
- Release the button to stop.
- The Arduino stops immediately when it receives STATE:0.
- The Arduino stops automatically after 3 seconds as a safety limit.
- After the 3-second safety stop, release all buttons before moving again.
- The web app repeats the current STATE while buttons are held so control stays reliable.

Notes
-----
- ENA and ENB are controlled by jumpers, not Arduino PWM pins.
- Motor speed control is not used in this setup.
- Turning is done by running one motor and stopping or reversing the other.
- If a motor spins in the wrong direction, swap that motor's two wires on the L298N output terminals.
- The service worker lets the controller UI load like an installed app.
- Bluetooth still requires a compatible browser and device.
