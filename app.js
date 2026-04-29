const SPP_UUID = '00001101-0000-1000-8000-00805f9b34fb';
const COMMAND_INTERVAL_MS = 120;
const SENSOR_STALE_MS = 2500;
const OBSTACLE_STOP_CM = 20;
const SW_CACHE_NAME = 'hc05-rc-pwa-v2';

const BIT_FORWARD = 1;
const BIT_BACKWARD = 2;
const BIT_LEFT = 4;
const BIT_RIGHT = 8;

const controlBits = {
  forward: BIT_FORWARD,
  backward: BIT_BACKWARD,
  left: BIT_LEFT,
  right: BIT_RIGHT,
};

const state = {
  forward: false,
  backward: false,
  left: false,
  right: false,
};

const pointerOwnership = new Map();
const buttonPointerMap = {
  forward: new Set(),
  backward: new Set(),
  left: new Set(),
  right: new Set(),
};

let port = null;
let writer = null;
let reader = null;
let readLoopRunning = false;
let commandTimer = null;
let lastSentMask = null;
let intentionallyDisconnecting = false;
let deferredInstallPrompt = null;
let lastSensorUpdateTime = 0;

const statusText = document.getElementById('statusText');
const statusDot = document.getElementById('statusDot');
const portName = document.getElementById('portName');
const lastCommand = document.getElementById('lastCommand');
const arduinoMessage = document.getElementById('arduinoMessage');
const appMode = document.getElementById('appMode');
const errorBox = document.getElementById('errorBox');
const connectBtn = document.getElementById('connectBtn');
const disconnectBtn = document.getElementById('disconnectBtn');
const installBtn = document.getElementById('installBtn');
const sensorCard = document.getElementById('sensorCard');
const frontDistance = document.getElementById('frontDistance');
const rearDistance = document.getElementById('rearDistance');
const sensorStatus = document.getElementById('sensorStatus');
const driveButtons = Array.from(document.querySelectorAll('.drive-button'));

initialize();

async function initialize() {
  updateAppMode();
  registerUiEvents();
  await registerServiceWorker();
  detectSerialSupport();
  await checkForPreviouslyGrantedPorts();
  setInterval(markSensorsStaleIfNeeded, 1000);
}

function registerUiEvents() {
  connectBtn.addEventListener('click', connectToCar);
  disconnectBtn.addEventListener('click', disconnectFromCar);
  installBtn.addEventListener('click', installPwa);

  for (const button of driveButtons) {
    const control = button.dataset.control;
    button.addEventListener('pointerdown', (event) => handlePointerDown(event, control));
    button.addEventListener('pointerup', (event) => handlePointerRelease(event));
    button.addEventListener('pointercancel', (event) => handlePointerRelease(event));
    button.addEventListener('lostpointercapture', (event) => handlePointerRelease(event));
    button.addEventListener('contextmenu', (event) => event.preventDefault());
  }

  window.addEventListener('pointerup', handleGlobalPointerRelease);
  window.addEventListener('pointercancel', handleGlobalPointerRelease);
  window.addEventListener('blur', emergencyStop);
  window.addEventListener('beforeunload', emergencyStop);
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
      emergencyStop();
    }
  });

  window.addEventListener('keydown', handleKeyDown);
  window.addEventListener('keyup', handleKeyUp);

  if ('serial' in navigator) {
    navigator.serial.addEventListener('disconnect', handleSerialDisconnectEvent);
  }

  window.addEventListener('beforeinstallprompt', (event) => {
    event.preventDefault();
    deferredInstallPrompt = event;
    installBtn.classList.remove('hidden');
  });

  window.addEventListener('appinstalled', () => {
    deferredInstallPrompt = null;
    installBtn.classList.add('hidden');
    updateAppMode();
  });
}

function updateAppMode() {
  const standalone = window.matchMedia('(display-mode: standalone)').matches || window.navigator.standalone === true;
  appMode.textContent = standalone ? 'Installed PWA' : 'Browser';
}

async function registerServiceWorker() {
  if (!('serviceWorker' in navigator)) {
    return;
  }

  try {
    await navigator.serviceWorker.register('./sw.js');
  } catch (error) {
    console.warn('Service worker registration failed:', error);
  }
}

function detectSerialSupport() {
  if (!('serial' in navigator)) {
    showError('This browser does not support Web Serial. Use a recent Chrome on Android.');
  }
}

async function checkForPreviouslyGrantedPorts() {
  if (!('serial' in navigator)) {
    return;
  }

  try {
    const ports = await navigator.serial.getPorts();
    if (ports.length > 0) {
      portName.textContent = 'Previously allowed device';
    }
  } catch (error) {
    console.warn('Could not query existing ports:', error);
  }
}

async function installPwa() {
  if (!deferredInstallPrompt) {
    return;
  }

  deferredInstallPrompt.prompt();
  try {
    await deferredInstallPrompt.userChoice;
  } catch (error) {
    console.warn('Install prompt dismissed:', error);
  }
  deferredInstallPrompt = null;
  installBtn.classList.add('hidden');
}

function handlePointerDown(event, control) {
  event.preventDefault();

  const button = event.currentTarget;
  try {
    button.setPointerCapture(event.pointerId);
  } catch (error) {
    // Safe to ignore.
  }

  pointerOwnership.set(event.pointerId, control);
  buttonPointerMap[control].add(event.pointerId);
  setControlActive(control, true);
}

function handlePointerRelease(event) {
  event.preventDefault();
  releasePointerOwner(event.pointerId);
}

function handleGlobalPointerRelease(event) {
  releasePointerOwner(event.pointerId);
}

function releasePointerOwner(pointerId) {
  const owner = pointerOwnership.get(pointerId);
  if (!owner) {
    return;
  }

  pointerOwnership.delete(pointerId);
  buttonPointerMap[owner].delete(pointerId);
  if (buttonPointerMap[owner].size === 0) {
    setControlActive(owner, false);
  }
}

function handleKeyDown(event) {
  if (event.repeat) {
    return;
  }

  const control = keyToControl(event.key);
  if (!control) {
    return;
  }

  event.preventDefault();
  setControlActive(control, true);
}

function handleKeyUp(event) {
  const control = keyToControl(event.key);
  if (!control) {
    return;
  }

  event.preventDefault();
  setControlActive(control, false);
}

function keyToControl(key) {
  switch (key.toLowerCase()) {
    case 'arrowup':
    case 'w':
      return 'forward';
    case 'arrowdown':
    case 's':
      return 'backward';
    case 'arrowleft':
    case 'a':
      return 'left';
    case 'arrowright':
    case 'd':
      return 'right';
    default:
      return null;
  }
}

function setControlActive(control, isActive) {
  if (state[control] === isActive) {
    return;
  }

  state[control] = isActive;
  updatePressedStyles();
  updateCommandLabel();
  sendCurrentState(true);
  updateHeartbeat();
}

function updatePressedStyles() {
  for (const button of driveButtons) {
    const control = button.dataset.control;
    button.classList.toggle('active', state[control]);
  }
}

function getMask() {
  let mask = 0;
  for (const [control, bit] of Object.entries(controlBits)) {
    if (state[control]) {
      mask |= bit;
    }
  }
  return mask;
}

function maskToLabel(mask) {
  if (mask === 0) {
    return 'STOP';
  }

  const labels = [];
  if (mask & BIT_FORWARD) labels.push('F');
  if (mask & BIT_BACKWARD) labels.push('B');
  if (mask & BIT_LEFT) labels.push('L');
  if (mask & BIT_RIGHT) labels.push('R');
  return labels.join('+');
}

function updateCommandLabel() {
  lastCommand.textContent = maskToLabel(getMask());
}

async function connectToCar() {
  clearError();

  if (!('serial' in navigator)) {
    showError('Web Serial is not available in this browser.');
    return;
  }

  if (port) {
    return;
  }

  connectBtn.disabled = true;
  intentionallyDisconnecting = false;
  setStatus('Connecting...', 'connecting');
  arduinoMessage.textContent = 'Connecting';

  try {
    port = await requestBluetoothSerialPort();
    await port.open({ baudRate: 9600, bufferSize: 256 });

    writer = port.writable.getWriter();
    setPortNameFromInfo();
    setStatus('Connected', 'connected');
    disconnectBtn.disabled = false;
    arduinoMessage.textContent = 'Connected';

    readFromArduino();
    await sendCurrentState(true);
    await requestSensorStatus();
  } catch (error) {
    await safeResetConnectionState();
    if (error && error.name === 'NotFoundError') {
      showError('No device was selected. Pair the HC-05 first, then tap Connect again.');
    } else {
      showError(`Could not connect: ${formatError(error)}`);
    }
  } finally {
    if (!port) {
      connectBtn.disabled = false;
    }
  }
}

async function requestBluetoothSerialPort() {
  try {
    return await navigator.serial.requestPort({
      allowedBluetoothServiceClassIds: [SPP_UUID],
      filters: [{ bluetoothServiceClassId: SPP_UUID }],
    });
  } catch (error) {
    const message = formatError(error).toLowerCase();
    const shouldFallback = error && error.name === 'TypeError' || message.includes('bluetoothserviceclassid');
    if (!shouldFallback) {
      throw error;
    }

    return navigator.serial.requestPort();
  }
}

async function disconnectFromCar() {
  intentionallyDisconnecting = true;
  await emergencyStop();
  await closeSerialConnection();
  setStatus('Disconnected', '');
  arduinoMessage.textContent = 'Disconnected';
  portName.textContent = 'None';
}

async function closeSerialConnection() {
  stopHeartbeat();

  if (reader) {
    try {
      await reader.cancel();
    } catch (error) {
      // Ignore read cancel errors.
    }
    reader.releaseLock();
    reader = null;
  }

  if (writer) {
    try {
      writer.releaseLock();
    } catch (error) {
      // Ignore writer release errors.
    }
    writer = null;
  }

  if (port) {
    try {
      await port.close();
    } catch (error) {
      // Ignore close errors.
    }
    port = null;
  }

  readLoopRunning = false;
  connectBtn.disabled = false;
  disconnectBtn.disabled = true;
}

async function readFromArduino() {
  if (!port || !port.readable || readLoopRunning) {
    return;
  }

  readLoopRunning = true;
  let textBuffer = '';
  const decoder = new TextDecoder();

  while (port && port.readable) {
    reader = port.readable.getReader();
    try {
      while (true) {
        const { value, done } = await reader.read();
        if (done) {
          break;
        }

        textBuffer += decoder.decode(value, { stream: true });
        let newlineIndex = -1;
        while ((newlineIndex = textBuffer.indexOf('\n')) !== -1) {
          const line = textBuffer.slice(0, newlineIndex).trim();
          textBuffer = textBuffer.slice(newlineIndex + 1);
          if (line) {
            handleArduinoMessage(line);
          }
        }
      }
    } catch (error) {
      if (!intentionallyDisconnecting) {
        showError(`Connection lost: ${formatError(error)}`);
      }
    } finally {
      reader.releaseLock();
      reader = null;
    }
    break;
  }

  readLoopRunning = false;

  if (port && !intentionallyDisconnecting) {
    await disconnectFromCar();
  }
}

function handleArduinoMessage(message) {
  if (message.startsWith('DIST:')) {
    updateSensorReadings(message);
    return;
  }

  if (message.startsWith('OBSTACLE:')) {
    handleObstacleMessage(message);
    return;
  }

  arduinoMessage.textContent = message;

  if (message === 'READY') {
    setStatus('Connected', 'connected');
    clearError();
    return;
  }

  if (message === 'SENSORS:ULTRASONIC_READY') {
    sensorStatus.textContent = 'Sensors ready';
    sensorCard.classList.remove('warning', 'stale');
    return;
  }

  if (message === 'SAFETY_LOCK') {
    clearAllControls();
    stopHeartbeat();
    sendCurrentState(true);
    updatePressedStyles();
    updateCommandLabel();
    showError('Safety auto-stop triggered after 3 seconds. Release all buttons, then press again to continue.', 'Safety stop');
    return;
  }

  if (message === 'UNLOCKED') {
    clearError();
    setStatus('Connected', 'connected');
  }
}

function updateSensorReadings(message) {
  const match = message.match(/^DIST:F=([^,]+),R=(.+)$/);
  if (!match) {
    return;
  }

  const frontCm = parseDistanceCm(match[1]);
  const rearCm = parseDistanceCm(match[2]);
  frontDistance.textContent = formatDistance(frontCm);
  rearDistance.textContent = formatDistance(rearCm);
  lastSensorUpdateTime = Date.now();

  sensorCard.classList.remove('stale');

  const frontClose = frontCm !== null && frontCm <= OBSTACLE_STOP_CM;
  const rearClose = rearCm !== null && rearCm <= OBSTACLE_STOP_CM;
  sensorCard.classList.toggle('warning', frontClose || rearClose);

  if (frontClose && rearClose) {
    sensorStatus.textContent = 'Front and rear obstacles detected';
  } else if (frontClose) {
    sensorStatus.textContent = 'Front obstacle detected';
  } else if (rearClose) {
    sensorStatus.textContent = 'Rear obstacle detected';
  } else if (frontCm === null && rearCm === null) {
    sensorStatus.textContent = 'No valid distance reading yet';
  } else {
    sensorStatus.textContent = 'Path clear';
  }
}

function parseDistanceCm(value) {
  const trimmed = value.trim();
  if (trimmed === '--') {
    return null;
  }

  const parsed = Number.parseInt(trimmed, 10);
  if (!Number.isFinite(parsed) || parsed < 0) {
    return null;
  }

  return parsed;
}

function formatDistance(distanceCm) {
  return distanceCm === null ? '--' : `${distanceCm} cm`;
}

function handleObstacleMessage(message) {
  arduinoMessage.textContent = message;
  const direction = message.split(':')[1] || '';

  if (direction === 'CLEAR') {
    sensorCard.classList.remove('warning');
    sensorStatus.textContent = 'Path clear';
    clearError();
    setStatus('Connected', 'connected');
    return;
  }

  clearAllControls();
  stopHeartbeat();
  sendCurrentState(true);

  sensorCard.classList.add('warning');
  const readableDirection = obstacleDirectionLabel(direction);
  sensorStatus.textContent = `${readableDirection} obstacle detected`;
  showError(`${readableDirection} obstacle detected. Motors stopped; steer away or reverse when clear.`, 'Obstacle stop');
}

function obstacleDirectionLabel(direction) {
  switch (direction) {
    case 'FRONT':
      return 'Front';
    case 'REAR':
      return 'Rear';
    case 'BOTH':
      return 'Front and rear';
    default:
      return 'Ultrasonic';
  }
}

function markSensorsStaleIfNeeded() {
  if (!lastSensorUpdateTime) {
    return;
  }

  if (Date.now() - lastSensorUpdateTime <= SENSOR_STALE_MS) {
    return;
  }

  sensorCard.classList.add('stale');
  if (!sensorCard.classList.contains('warning')) {
    sensorStatus.textContent = 'Sensor telemetry paused';
  }
}

async function requestSensorStatus() {
  if (!writer) {
    return;
  }

  try {
    await writer.write(new TextEncoder().encode('SENSOR?\n'));
  } catch (error) {
    console.warn('Sensor status request failed:', error);
  }
}

async function sendCurrentState(forceSend = false) {
  const mask = getMask();
  const payload = `STATE:${mask}\n`;

  if (!forceSend && lastSentMask === mask) {
    return;
  }

  lastSentMask = mask;

  if (!writer) {
    return;
  }

  try {
    await writer.write(new TextEncoder().encode(payload));
  } catch (error) {
    showError(`Send failed: ${formatError(error)}`);
  }
}

function updateHeartbeat() {
  const moving = getMask() !== 0;

  if (moving && !commandTimer) {
    commandTimer = setInterval(() => {
      sendCurrentState(true);
    }, COMMAND_INTERVAL_MS);
    return;
  }

  if (!moving) {
    stopHeartbeat();
  }
}

function stopHeartbeat() {
  if (commandTimer) {
    clearInterval(commandTimer);
    commandTimer = null;
  }
}

function clearAllControls() {
  for (const key of Object.keys(state)) {
    state[key] = false;
  }
  for (const set of Object.values(buttonPointerMap)) {
    set.clear();
  }
  pointerOwnership.clear();
  updatePressedStyles();
  updateCommandLabel();
}

async function emergencyStop() {
  clearAllControls();
  stopHeartbeat();
  await sendCurrentState(true);
}

function setStatus(text, mode) {
  statusText.textContent = text;
  statusDot.className = 'dot';
  if (mode) {
    statusDot.classList.add(mode);
  }
}

function setPortNameFromInfo() {
  if (!port) {
    portName.textContent = 'None';
    return;
  }

  try {
    const info = port.getInfo();
    if (info.bluetoothServiceClassId) {
      portName.textContent = 'Bluetooth SPP';
      return;
    }
    if (info.usbVendorId) {
      portName.textContent = `USB 0x${info.usbVendorId.toString(16)}`;
      return;
    }
  } catch (error) {
    // Ignore and fall back below.
  }

  portName.textContent = 'Serial device';
}

async function handleSerialDisconnectEvent() {
  if (!port || intentionallyDisconnecting) {
    return;
  }

  showError('Bluetooth connection was disconnected.');
  await safeResetConnectionState();
}

async function safeResetConnectionState() {
  await closeSerialConnection();
  clearAllControls();
  setStatus('Disconnected', '');
  arduinoMessage.textContent = 'Disconnected';
  portName.textContent = 'None';
}

function formatError(error) {
  return error && error.message ? error.message : String(error);
}

function showError(message, statusLabel = 'Error') {
  errorBox.textContent = message;
  errorBox.classList.add('show');
  statusText.textContent = statusLabel;
  statusDot.className = 'dot error';
}

function clearError() {
  errorBox.textContent = '';
  errorBox.classList.remove('show');
}
