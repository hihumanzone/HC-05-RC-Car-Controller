HC-05 RC Car PWA
================

Files
-----
- index.html              Main controller page
- styles.css              UI styling
- app.js                  PWA + Bluetooth logic
- manifest.webmanifest    PWA manifest
- sw.js                   Service worker for offline installable shell
- icons/                  App icons
- hc05_rc_car.ino         Matching Arduino sketch

How to use
----------
1. Upload hc05_rc_car.ino to your Arduino Uno.
2. Keep the HC-05 and L298N wiring unchanged.
3. Pair the HC-05 in Android Bluetooth settings.
4. Serve this folder over HTTPS.
   Examples:
   - GitHub Pages
   - Netlify
   - Any HTTPS web host
5. Open index.html in Chrome on Android.
6. Tap Connect and choose the HC-05 serial device.
7. Install the PWA with the Install App button or Chrome's Add to Home Screen option.

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
- STATE:5  = Forward + Left
- STATE:9  = Forward + Right
- STATE:0  = Stop

Notes
-----
- The Arduino stops immediately when it receives STATE:0.
- The Arduino also stops automatically after 3 seconds as a safety limit.
- The web app repeats the current STATE while buttons are held so control stays reliable.
- The service worker lets the controller UI load like an installed app, but Bluetooth still requires a compatible browser and device.
