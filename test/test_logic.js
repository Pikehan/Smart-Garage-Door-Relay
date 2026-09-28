/**
 * AUTOMATED TEST SUITE FOR ESP32 GARAGE DOOR CONTROLLER
 * Transpiles and tests C++ functions directly from src/main.cpp
 */

const fs = require('fs');
const path = require('path');

// Read src/door_logic.cpp
const cppPath = path.join(__dirname, '..', 'src', 'door_logic.cpp');
if (!fs.existsSync(cppPath)) {
  console.error('❌ Error: src/door_logic.cpp file not found at ' + cppPath);
  process.exit(1);
}

const cppCode = fs.readFileSync(cppPath, 'utf8');

// --- SIMULATED HARDWARE ENVIRONMENT ---
let RELAY_PIN = 2;
let OPEN_SENSOR_PIN = 25;
let CLOSED_SENSOR_PIN = 26;
let MOVEMENT_TIMEOUT = 27000;
let DEBOUNCE_DELAY = 50;
let RELAY_PRESS_TIME = 200;
let PENDING_RELAY_DELAY = 500;

let COMMAND_LOCKOUT_MS = 1500;
let SENSOR_DISENGAGE_TIMEOUT = 2500;

// Calibration & Position Tracking parameters
let DEFAULT_OPEN_DURATION_MS = 17000;
let DEFAULT_CLOSE_DURATION_MS = 17000;
let MIN_TRAVEL_TIME_MS = 15000;
let MAX_TRAVEL_TIME_MS = 25000;
let CALIBRATION_NVS_MIN_DELTA_MS = 200;
let CALIBRATION_MAX_DEVIATION_MS = 3500;
let CALIBRATION_INTERVAL_MS = 86400000;
let NVS_WRITE_COOLDOWN_MS = 300000;

// Enums
const STATE_UNKNOWN = 0;
const STATE_CLOSED = 1;
const STATE_OPENING = 2;
const STATE_CLOSING = 3;
const STATE_OPEN = 4;
const STATE_STOPPED = 5;

// Variables
let currentState = STATE_UNKNOWN;
let previousState = STATE_UNKNOWN;
let lastStateChangeTime = Date.now();
let realClosedSensor = false;
let realOpenSensor = false;
let lastReadingClosed = false;
let lastReadingOpen = false;
let relayActive = false;
let relayTriggerTime = 0;
let pendingState = STATE_UNKNOWN;
let pendingPulsesCount = 0;

// Telemetry & Diagnostic Variables
let obstacleWarning = false;
let sensorFault = false;
let sensorTimeoutError = false;
let failedToMove = false;
let midTrackStall = false;
let lastCommandedDirection = STATE_UNKNOWN;
let lastPulseTime = 0;
let pulseVerificationPending = false;
let pulseOriginState = STATE_UNKNOWN;

// Calibration & Position Tracking variables
let openDurationMs = 17000;
let closeDurationMs = 17000;
let nvsStoredOpenMs = 17000;
let nvsStoredCloseMs = 17000;
let isCalibrated = false;
let currentPositionPct = 0;
let startPositionPct = 0;
let activeFlightStartTime = 0;
let switchUnseated = false;
let lastCalibrationTimeOpen = 0;
let lastCalibrationTimeClose = 0;
let lastNVSWriteTime = 0;
let hasIntermediateStop = false;

let pinRelayState = 1; // 1 = HIGH (OFF), 0 = LOW (ON)
let pinOpenSensor = 1; // 1 = Inactive, 0 = Active
let pinClosedSensor = 1; // 1 = Inactive, 0 = Active

let pulseLog = [];

function digitalWrite(pin, val) {
  if (pin === RELAY_PIN) {
    if (global.pinRelayState === 1 && val === 0) {
      pulseLog.push(Date.now());
    }
    global.pinRelayState = val;
  }
}

function digitalRead(pin) {
  if (pin === OPEN_SENSOR_PIN) return global.pinOpenSensor !== undefined ? global.pinOpenSensor : pinOpenSensor;
  if (pin === CLOSED_SENSOR_PIN) return global.pinClosedSensor !== undefined ? global.pinClosedSensor : pinClosedSensor;
  return 1;
}

function millis() {
  if (global.simulatedMillis !== undefined) return global.simulatedMillis;
  return Date.now();
}

function saveStateToNVS() {}
function sendCORSHeaders() {}
function clearFaultFlags() {
  global.obstacleWarning = false;
  global.sensorFault = false;
  global.sensorTimeoutError = false;
  global.failedToMove = false;
  global.midTrackStall = false;
}

let lastServerResponse = null;

const server = {
  send: function(code, type, body) {
    lastServerResponse = { code, type, body };
    return lastServerResponse;
  }
};

const Serial = {
  begin: function() {},
  println: function() {},
  printf: function() {},
  print: function() {}
};

function getDebugString(s) {
  switch(s) {
    case STATE_CLOSED: return "Closed";
    case STATE_OPENING: return "Opening";
    case STATE_CLOSING: return "Closing";
    case STATE_OPEN: return "Open";
    case STATE_STOPPED: return "Stopped";
    default: return "Unknown";
  }
}

global.RELAY_PIN = RELAY_PIN;
global.OPEN_SENSOR_PIN = OPEN_SENSOR_PIN;
global.CLOSED_SENSOR_PIN = CLOSED_SENSOR_PIN;
global.MOVEMENT_TIMEOUT = MOVEMENT_TIMEOUT;
global.DEBOUNCE_DELAY = DEBOUNCE_DELAY;
global.RELAY_PRESS_TIME = RELAY_PRESS_TIME;
global.PENDING_RELAY_DELAY = PENDING_RELAY_DELAY;
global.COMMAND_LOCKOUT_MS = COMMAND_LOCKOUT_MS;
global.SENSOR_DISENGAGE_TIMEOUT = SENSOR_DISENGAGE_TIMEOUT;

global.STATE_UNKNOWN = STATE_UNKNOWN;
global.STATE_CLOSED = STATE_CLOSED;
global.STATE_OPENING = STATE_OPENING;
global.STATE_CLOSING = STATE_CLOSING;
global.STATE_OPEN = STATE_OPEN;
global.STATE_STOPPED = STATE_STOPPED;

global.currentState = currentState;
global.previousState = previousState;
global.lastStateChangeTime = lastStateChangeTime;
global.realClosedSensor = realClosedSensor;
global.realOpenSensor = realOpenSensor;
global.lastReadingClosed = lastReadingClosed;
global.lastReadingOpen = lastReadingOpen;
global.relayActive = relayActive;
global.relayTriggerTime = relayTriggerTime;
global.pendingState = pendingState;
global.pendingPulsesCount = pendingPulsesCount;

global.obstacleWarning = obstacleWarning;
global.sensorFault = sensorFault;
global.sensorTimeoutError = sensorTimeoutError;
global.failedToMove = failedToMove;
global.midTrackStall = midTrackStall;
global.lastCommandedDirection = lastCommandedDirection;
global.lastPulseTime = lastPulseTime;
global.pulseVerificationPending = pulseVerificationPending;
global.pulseOriginState = pulseOriginState;

global.DEFAULT_OPEN_DURATION_MS = DEFAULT_OPEN_DURATION_MS;
global.DEFAULT_CLOSE_DURATION_MS = DEFAULT_CLOSE_DURATION_MS;
global.MIN_TRAVEL_TIME_MS = MIN_TRAVEL_TIME_MS;
global.MAX_TRAVEL_TIME_MS = MAX_TRAVEL_TIME_MS;
global.CALIBRATION_NVS_MIN_DELTA_MS = CALIBRATION_NVS_MIN_DELTA_MS;
global.CALIBRATION_MAX_DEVIATION_MS = CALIBRATION_MAX_DEVIATION_MS;
global.CALIBRATION_INTERVAL_MS = CALIBRATION_INTERVAL_MS;
global.NVS_WRITE_COOLDOWN_MS = NVS_WRITE_COOLDOWN_MS;

global.openDurationMs = openDurationMs;
global.closeDurationMs = closeDurationMs;
global.nvsStoredOpenMs = nvsStoredOpenMs;
global.nvsStoredCloseMs = nvsStoredCloseMs;
global.isCalibrated = isCalibrated;
global.currentPositionPct = currentPositionPct;
global.startPositionPct = startPositionPct;
global.activeFlightStartTime = activeFlightStartTime;
global.switchUnseated = switchUnseated;
global.lastCalibrationTimeOpen = lastCalibrationTimeOpen;
global.lastCalibrationTimeClose = lastCalibrationTimeClose;
global.lastNVSWriteTime = lastNVSWriteTime;
global.hasIntermediateStop = hasIntermediateStop;

global.digitalWrite = digitalWrite;
global.digitalRead = digitalRead;
global.millis = millis;
global.sendCORSHeaders = sendCORSHeaders;
global.clearFaultFlags = clearFaultFlags;
global.server = server;
global.Serial = Serial;
global.getDebugString = getDebugString;

global.nvsStore = {
  curr: STATE_STOPPED,
  prev: STATE_UNKNOWN,
  s_close: false,
  s_open: false
};


// C++ Transpiler Helper
function transpileBodyToJS(cppBody) {
  let js = cppBody;
  js = js.replace(/\/\/#.*/g, '');
  js = js.replace(/\/\/.*$/gm, '');
  js = js.replace(/\bconst\s+char\s+PROGMEM\s+updatePage\[\]\s*=\s*[\s\S]*?;\s*/g, '');
  js = js.replace(/\bWiFi\.mode[\s\S]*/, '');
  js = js.replace(/server\.on\s*\([\s\S]*?\);\s*/g, '');
  js = js.replace(/server\.onNotFound\s*\([^;]*\);\s*/g, '');
  js = js.replace(/server\.sendHeader\s*\([^;]*\);?/g, '');
  js = js.replace(/server\.send\s*\(/g, 'global.server.send(');
  js = js.replace(/(?:WiFi|ArduinoOTA)\.[^;]+;/g, '');
  js = js.replace(/(?:pinMode|setupOTA)\s*\([^;]*\);?/g, '');
  js = js.replace(/\((?:uint8_t|DoorState|int|uint16_t|unsigned\s+int|uint32_t|unsigned\s+long|long)\)/g, '');
  js = js.replace(/\b(\d+)UL\b/g, '$1');
  js = js.replace(/\blabs\b/g, 'Math.abs');
  js = js.replace(/\b(?:unsigned\s+long|long|int|bool|uint8_t|DoorState|char\*|const\s+char\*|String)\s+/g, 'let ');
  js = js.replace(/^[ \t]*#(?:if|ifdef|ifndef|elif|else|endif).*$/gm, '');
  js = js.replace(/(?:preferences|prefs)\.getUChar\s*\(\s*"([^"]+)"\s*,\s*([^)]+)\)/g, '(global.nvsStore["$1"] !== undefined ? global.nvsStore["$1"] : $2)');
  js = js.replace(/(?:preferences|prefs)\.getBool\s*\(\s*"([^"]+)"\s*,\s*([^)]+)\)/g, '(global.nvsStore["$1"] !== undefined ? global.nvsStore["$1"] : $2)');
  js = js.replace(/(?:preferences|prefs)\.getUInt\s*\(\s*"([^"]+)"\s*,\s*([^)]+)\)/g, '(global.nvsStore["$1"] !== undefined ? global.nvsStore["$1"] : $2)');
  js = js.replace(/(?:preferences|prefs)\.putUChar\s*\(\s*"([^"]+)"\s*,\s*([^)]+)\)/g, '(global.nvsStore["$1"] = $2)');
  js = js.replace(/(?:preferences|prefs)\.putBool\s*\(\s*"([^"]+)"\s*,\s*([^)]+)\)/g, '(global.nvsStore["$1"] = $2)');
  js = js.replace(/(?:preferences|prefs)\.putUInt\s*\(\s*"([^"]+)"\s*,\s*([^)]+)\)/g, '(global.nvsStore["$1"] = $2)');
  js = js.replace(/(?:preferences|prefs)\.(?:begin|end)\s*\([^;]*\);?/g, '');
  js = js.replace(/Preferences\s+\w+;?/g, '');
  js = js.replace(/if\s*\(!isSameOriginRequest\(\)\)\s*\{[\s\S]*?return;\s*\}/g, '');
  js = js.replace(/if\s*\(!server\.authenticate\([^)]*\)\)\s*\{[\s\S]*?return;\s*\}/g, '');
  js = js.replace(/DoorStateLock\s+\w+(?:\([^)]*\))?;?/g, '');
  js = js.replace(/if\s*\(!lock\.acquired\)\s*\{[\s\S]*?return;\s*\}/g, '');
  js = js.replace(/if\s*\(\s*doorStateMutex[\s\S]*?\}/g, '');
  js = js.replace(/xSemaphore(?:CreateRecursiveMutex|TakeRecursive|GiveRecursive)\s*\([^;]*\);?/g, '');
  js = js.replace(/doorStateMutex\s*=\s*[^;]+;/g, '');
  js = js.replace(/\bHIGH\b/g, '1').replace(/\bLOW\b/g, '0');
  return js;
}

function extractFunctionBody(code, funcName) {
  const pattern = new RegExp('\\b(?:void|DoorState|int|bool|String|uint8_t)\\s+' + funcName + '\\s*\\([^)]*\\)\\s*\\{');
  const match = code.match(pattern);
  if (!match) return null;

  let startIndex = match.index + match[0].length - 1;
  let depth = 0;
  let endIndex = -1;

  for (let i = startIndex; i < code.length; i++) {
    if (code[i] === '{') depth++;
    else if (code[i] === '}') {
      depth--;
      if (depth === 0) {
        endIndex = i;
        break;
      }
    }
  }

  if (endIndex === -1) return null;
  return code.substring(startIndex + 1, endIndex);
}

function compileFunc(funcName) {
  const targetName = (funcName === 'setup') ? 'initDoorHardware' : funcName;
  const body = extractFunctionBody(cppCode, targetName) || extractFunctionBody(cppCode, funcName);
  if (!body) throw new Error(`Could not find function ${funcName} (or ${targetName}) in src/door_logic.cpp`);
  const jsCode = transpileBodyToJS(body);
  return new Function(jsCode);
}

// Dynamic JS Bindings
global.triggerRelay = function() { fnTriggerRelay(); };
global.handleRelay = function() { fnHandleRelay(); };
global.handleOn = function() { return fnHandleOn(); };
global.handleOff = function() { return fnHandleOff(); };
global.handleToggle = function() { return fnHandleToggle(); };
global.checkSensorsWithDebounce = function() { fnCheckSensors(); };
global.updateLogic = function() { fnUpdateLogic(); };
global.setup = function() { fnSetup(); };
global.calculateCurrentPosition = function() { return fnCalculateCurrentPosition(); };
global.handleCalibrateReset = function() { return fnHandleCalibrateReset(); };
global.saveStateToNVS = function() { fnSaveStateToNVS(); };
global.releaseRelayIfExpired = function() { fnReleaseRelayIfExpired(); };
global.isSameOriginRequest = function() { return true; };

const fnTriggerRelay = compileFunc('triggerRelay');
const fnHandleRelay = compileFunc('handleRelay');
const fnHandleOn = compileFunc('handleOn');
const fnHandleOff = compileFunc('handleOff');
const fnHandleToggle = compileFunc('handleToggle');
const fnCheckSensors = compileFunc('checkSensorsWithDebounce');
const fnUpdateLogic = compileFunc('updateLogic');
const fnSetup = compileFunc('setup');
const fnCalculateCurrentPosition = compileFunc('calculateCurrentPosition');
const fnHandleCalibrateReset = compileFunc('handleCalibrateReset');
const fnSaveStateToNVS = compileFunc('saveStateToNVS');
const fnReleaseRelayIfExpired = compileFunc('releaseRelayIfExpired');

function triggerRelay() { global.triggerRelay(); }
function handleRelay() { global.handleRelay(); }
function handleOn() { return global.handleOn(); }
function handleOff() { return global.handleOff(); }
function handleToggle() { return global.handleToggle(); }
function checkSensorsWithDebounce() { global.checkSensorsWithDebounce(); }
function updateLogic() { global.updateLogic(); }
function setup() { global.setup(); }
function calculateCurrentPosition() { return global.calculateCurrentPosition(); }
function handleCalibrateReset() { return global.handleCalibrateReset(); }
function saveStateToNVS() { global.saveStateToNVS(); }
function releaseRelayIfExpired() { global.releaseRelayIfExpired(); }
function isSameOriginRequest() { return true; }

// --- TEST RUNNER ENGINE ---
let totalTests = 0;
let passedTests = 0;
let failedTests = 0;

function assert(condition, testName, failureDetails) {
  totalTests++;
  if (condition) {
    passedTests++;
    console.log(`  ✅ PASS: ${testName}`);
  } else {
    failedTests++;
    console.error(`  ❌ FAIL: ${testName} -> ${failureDetails}`);
  }
}

function resetEnvironment() {
  global.currentState = STATE_UNKNOWN;
  global.previousState = STATE_UNKNOWN;
  global.lastStateChangeTime = millis();
  global.realClosedSensor = false;
  global.realOpenSensor = false;
  global.lastReadingClosed = false;
  global.lastReadingOpen = false;
  global.relayActive = false;
  global.relayTriggerTime = 0;
  global.pendingState = STATE_UNKNOWN;
  global.pendingPulsesCount = 0;
  global.obstacleWarning = false;
  global.sensorFault = false;
  global.sensorTimeoutError = false;
  global.failedToMove = false;
  global.midTrackStall = false;
  global.lastCommandedDirection = STATE_UNKNOWN;
  global.lastPulseTime = 0;
  global.pulseVerificationPending = false;
  global.pulseOriginState = STATE_UNKNOWN;

  global.openDurationMs = 17000;
  global.closeDurationMs = 17000;
  global.nvsStoredOpenMs = 17000;
  global.nvsStoredCloseMs = 17000;
  global.isCalibrated = false;
  global.currentPositionPct = 0;
  global.startPositionPct = 0;
  global.activeFlightStartTime = 0;
  global.switchUnseated = false;
  global.lastCalibrationTimeOpen = 0;
  global.lastCalibrationTimeClose = 0;
  global.lastNVSWriteTime = 0;
  global.hasIntermediateStop = false;
  delete global.simulatedMillis;

  global.pinRelayState = 1;
  global.pinOpenSensor = 1;
  global.pinClosedSensor = 1;
  pinRelayState = 1;
  pulseLog = [];
  lastServerResponse = null;
}


console.log('====================================================');
console.log('  🧪 ESP32 GARAGE DOOR CONTROLLER AUTOMATED TESTS  ');
console.log('====================================================\n');

// --- TEST CASES FOR USER SPECIFICATION ---

// Specification 1: "when I press open if its closed state it should open the door by just sending one relay pulse"
(() => {
  resetEnvironment();
  global.currentState = STATE_CLOSED;
  global.previousState = STATE_CLOSED;
  global.pinClosedSensor = 0;
  global.realClosedSensor = true;

  handleOn();
  assert(global.currentState === STATE_OPENING, 'Spec 1.1: When CLOSED, pressing OPEN starts OPENING the door', `got ${getDebugString(global.currentState)}`);
  assert(pulseLog.length === 1, 'Spec 1.2: When CLOSED, pressing OPEN sends just 1 relay pulse', `got ${pulseLog.length} pulses`);
})();

// Specification 2: "if its opening when pressed open it should not do anything"
(() => {
  resetEnvironment();
  global.currentState = STATE_OPENING;
  global.previousState = STATE_CLOSED;
  
  handleOn();
  assert(lastServerResponse && lastServerResponse.body && lastServerResponse.body.includes('ignored'), 'Spec 2.1: When OPENING, pressing OPEN does not do anything (returns IGNORED)', JSON.stringify(lastServerResponse));
  assert(pulseLog.length === 0, 'Spec 2.2: When OPENING, pressing OPEN sends 0 relay pulses', `got ${pulseLog.length} pulses`);
})();

// Specification 3: "if its closing state it shoud send one pulse to stop the closing door wait 500 ms and one pulse to open again"
(() => {
  resetEnvironment();
  global.currentState = STATE_CLOSING;
  global.previousState = STATE_OPEN;

  handleOn();
  assert(global.currentState === STATE_STOPPED, 'Spec 3.1: When CLOSING, pressing OPEN sends 1st pulse immediately to STOP', `got ${getDebugString(global.currentState)}`);
  assert(global.pendingState === STATE_OPENING, 'Spec 3.2: When CLOSING, pressing OPEN queues pending state to OPENING', `got ${getDebugString(global.pendingState)}`);
  assert(pulseLog.length === 1, 'Spec 3.3: 1st pulse sent immediately', `got ${pulseLog.length} pulses`);

  // Simulate 500ms delay
  global.relayTriggerTime = millis() - 600;
  global.relayActive = false;
  global.pinRelayState = 1;
  handleRelay();

  assert(global.currentState === STATE_OPENING, 'Spec 3.4: After waiting 500ms, state transitions to OPENING', `got ${getDebugString(global.currentState)}`);
  assert(pulseLog.length === 2, 'Spec 3.5: After waiting 500ms, 2nd pulse sent to OPEN again', `got ${pulseLog.length} pulses`);
})();

// Specification 4: "when I press close if its open state it should close the door by just sending one relay pulse"
(() => {
  resetEnvironment();
  global.currentState = STATE_OPEN;
  global.previousState = STATE_OPEN;

  handleOff();
  assert(global.currentState === STATE_CLOSING, 'Spec 4.1: When OPEN, pressing CLOSE starts CLOSING the door', `got ${getDebugString(global.currentState)}`);
  assert(pulseLog.length === 1, 'Spec 4.2: When OPEN, pressing CLOSE sends just 1 relay pulse', `got ${pulseLog.length} pulses`);
})();

// Specification 5: "if its closing when pressed close it should not do anything"
(() => {
  resetEnvironment();
  global.currentState = STATE_CLOSING;
  global.previousState = STATE_OPEN;

  handleOff();
  assert(lastServerResponse && lastServerResponse.body && lastServerResponse.body.includes('ignored'), 'Spec 5.1: When CLOSING, pressing CLOSE does not do anything (returns IGNORED)', JSON.stringify(lastServerResponse));
  assert(pulseLog.length === 0, 'Spec 5.2: When CLOSING, pressing CLOSE sends 0 relay pulses', `got ${pulseLog.length} pulses`);
})();

// Specification 6: "if its opening state it shoud send one pulse to stop the opening door wait 500 ms and one pulse to close again"
(() => {
  resetEnvironment();
  global.currentState = STATE_OPENING;
  global.previousState = STATE_CLOSED;

  handleOff();
  assert(global.currentState === STATE_STOPPED, 'Spec 6.1: When OPENING, pressing CLOSE sends 1st pulse immediately to STOP', `got ${getDebugString(global.currentState)}`);
  assert(global.pendingState === STATE_CLOSING, 'Spec 6.2: When OPENING, pressing CLOSE queues pending state to CLOSING', `got ${getDebugString(global.pendingState)}`);
  assert(pulseLog.length === 1, 'Spec 6.3: 1st pulse sent immediately', `got ${pulseLog.length} pulses`);

  // Simulate 500ms delay
  global.relayTriggerTime = millis() - 600;
  global.relayActive = false;
  global.pinRelayState = 1;
  handleRelay();

  assert(global.currentState === STATE_CLOSING, 'Spec 6.4: After waiting 500ms, state transitions to CLOSING', `got ${getDebugString(global.currentState)}`);
  assert(pulseLog.length === 2, 'Spec 6.5: After waiting 500ms, 2nd pulse sent to CLOSE again', `got ${pulseLog.length} pulses`);
})();

// Test 6: POST /toggle cancels pending auto-reverse
(() => {
  resetEnvironment();
  global.currentState = STATE_STOPPED;
  global.pendingState = STATE_OPENING;
  global.pendingPulsesCount = 1;

  handleToggle();
  assert(global.pendingState === STATE_UNKNOWN, 'Test 6.1: handleToggle() clears pendingState to UNKNOWN', `got ${global.pendingState}`);
  assert(global.pendingPulsesCount === 0, 'Test 6.2: handleToggle() resets pendingPulsesCount to 0', `got ${global.pendingPulsesCount}`);
  assert(global.currentState === STATE_STOPPED, 'Test 6.3: Door remains STOPPED upon cancelling pending action', `got ${getDebugString(global.currentState)}`);
})();

// Test 7: Self-Healing Limit Switch Activation
(() => {
  resetEnvironment();
  global.currentState = STATE_OPENING;
  global.previousState = STATE_STOPPED;
  global.realClosedSensor = true; // Unexpected closed sensor trip

  updateLogic();
  assert(global.currentState === STATE_CLOSED, 'Test 7.1: updateLogic() self-heals STATE_OPENING to STATE_CLOSED on Closed sensor', `got ${getDebugString(global.currentState)}`);
})();

// Test 8: Post-Reboot Leaving Sensor Protection
(() => {
  resetEnvironment();
  global.currentState = STATE_OPENING;
  global.previousState = STATE_CLOSED;
  global.realClosedSensor = true; // Magnet still leaving sensor switch

  updateLogic();
  assert(global.currentState === STATE_OPENING, 'Test 8.1: updateLogic() ignores leaving sensor contact and preserves OPENING', `got ${getDebugString(global.currentState)}`);
})();

// Test 9: Movement Safety Timeout (27 Seconds)
(() => {
  resetEnvironment();
  global.currentState = STATE_OPENING;
  global.previousState = STATE_CLOSED;
  global.lastStateChangeTime = millis() - (MOVEMENT_TIMEOUT + 1000); // Exceeded movement timeout

  updateLogic();
  assert(global.currentState === STATE_STOPPED, 'Test 9.1: updateLogic() forces STOPPED after movement timeout', `got ${getDebugString(global.currentState)}`);
})();

// Test 10: Hardware Fault Protection (Dual Sensors Active)
(() => {
  resetEnvironment();
  global.currentState = STATE_OPENING;
  global.realClosedSensor = true;
  global.realOpenSensor = true;

  updateLogic();
  assert(global.currentState === STATE_STOPPED, 'Test 10.1: updateLogic() forces STOPPED when both limit switches read active', `got ${getDebugString(global.currentState)}`);
})();

// Test 11: handleOn() when STOPPED after OPENING (3-pulse sequence)
(() => {
  resetEnvironment();
  global.currentState = STATE_STOPPED;
  global.previousState = STATE_OPENING;

  handleOn();
  assert(pulseLog.length === 1, 'Test 11.1: handleOn() from STOPPED (was OPENING) sends 1st pulse immediately', `got ${pulseLog.length}`);
  assert(global.pendingState === STATE_OPENING, 'Test 11.2: Queues pendingState to OPENING', `got ${getDebugString(global.pendingState)}`);
  assert(global.pendingPulsesCount === 2, 'Test 11.3: Sets pendingPulsesCount to 2', `got ${global.pendingPulsesCount}`);

  // Simulate 500ms delay -> Pulse 2 (stop closing)
  global.relayTriggerTime = millis() - 600;
  global.relayActive = false;
  global.pinRelayState = 1;
  handleRelay();

  assert(pulseLog.length === 2, 'Test 11.4: Sends 2nd pulse after 500ms to stop opposite movement', `got ${pulseLog.length}`);
  assert(global.pendingPulsesCount === 1, 'Test 11.5: Decrements pendingPulsesCount to 1', `got ${global.pendingPulsesCount}`);

  // Simulate another 500ms delay -> Pulse 3 (start opening)
  global.relayTriggerTime = millis() - 600;
  global.relayActive = false;
  global.pinRelayState = 1;
  handleRelay();

  assert(pulseLog.length === 3, 'Test 11.6: Sends 3rd pulse after 500ms to start desired opening', `got ${pulseLog.length}`);
  assert(global.currentState === STATE_OPENING, 'Test 11.7: State transitions to OPENING', `got ${getDebugString(global.currentState)}`);
  assert(global.pendingState === STATE_UNKNOWN, 'Test 11.8: Clears pendingState to UNKNOWN', `got ${global.pendingState}`);
})();

// Test 12: handleOn() when STOPPED after CLOSING (1-pulse sequence)
(() => {
  resetEnvironment();
  global.currentState = STATE_STOPPED;
  global.previousState = STATE_CLOSING;

  handleOn();
  assert(pulseLog.length === 1, 'Test 12.1: handleOn() from STOPPED (was CLOSING) sends 1 pulse to open', `got ${pulseLog.length}`);
  assert(global.currentState === STATE_OPENING, 'Test 12.2: State transitions immediately to OPENING', `got ${getDebugString(global.currentState)}`);
})();

// Test 13: handleOff() when STOPPED after CLOSING (3-pulse sequence)
(() => {
  resetEnvironment();
  global.currentState = STATE_STOPPED;
  global.previousState = STATE_CLOSING;

  handleOff();
  assert(pulseLog.length === 1, 'Test 13.1: handleOff() from STOPPED (was CLOSING) sends 1st pulse immediately', `got ${pulseLog.length}`);
  assert(global.pendingState === STATE_CLOSING, 'Test 13.2: Queues pendingState to CLOSING', `got ${getDebugString(global.pendingState)}`);
  assert(global.pendingPulsesCount === 2, 'Test 13.3: Sets pendingPulsesCount to 2', `got ${global.pendingPulsesCount}`);

  // Simulate 500ms delay -> Pulse 2 (stop opening)
  global.relayTriggerTime = millis() - 600;
  global.relayActive = false;
  global.pinRelayState = 1;
  handleRelay();

  assert(pulseLog.length === 2, 'Test 13.4: Sends 2nd pulse after 500ms to stop opposite movement', `got ${pulseLog.length}`);
  assert(global.pendingPulsesCount === 1, 'Test 13.5: Decrements pendingPulsesCount to 1', `got ${global.pendingPulsesCount}`);

  // Simulate another 500ms delay -> Pulse 3 (start closing)
  global.relayTriggerTime = millis() - 600;
  global.relayActive = false;
  global.pinRelayState = 1;
  handleRelay();

  assert(pulseLog.length === 3, 'Test 13.6: Sends 3rd pulse after 500ms to start desired closing', `got ${pulseLog.length}`);
  assert(global.currentState === STATE_CLOSING, 'Test 13.7: State transitions to CLOSING', `got ${getDebugString(global.currentState)}`);
  assert(global.pendingState === STATE_UNKNOWN, 'Test 13.8: Clears pendingState to UNKNOWN', `got ${global.pendingState}`);
})();

// Test 14: handleOff() when STOPPED after OPENING (1-pulse sequence)
(() => {
  resetEnvironment();
  global.currentState = STATE_STOPPED;
  global.previousState = STATE_OPENING;

  handleOff();
  assert(pulseLog.length === 1, 'Test 14.1: handleOff() from STOPPED (was OPENING) sends 1 pulse to close', `got ${pulseLog.length}`);
  assert(global.currentState === STATE_CLOSING, 'Test 14.2: State transitions immediately to CLOSING', `got ${getDebugString(global.currentState)}`);
})();

// Test 15: handleToggle() cancelling 3-pulse sequence while motor is actively moving
(() => {
  resetEnvironment();
  global.currentState = STATE_STOPPED;
  global.previousState = STATE_OPENING;

  handleOn(); // Pulse 1 sent (starts motor closing)
  assert(pulseLog.length === 1, 'Test 15.1: Pulse 1 fired');
  assert(global.pendingPulsesCount === 2, 'Test 15.2: 2 pulses remaining');

  // Relay releases after 200ms
  global.relayTriggerTime = millis() - 250;
  global.relayActive = false;
  global.pinRelayState = 1;

  // User hits toggle to cancel while motor is actively closing mid-interval
  handleToggle();
  assert(pulseLog.length === 2, 'Test 15.3: handleToggle() sends pulse to STOP active motor', `got ${pulseLog.length}`);
  assert(global.currentState === STATE_STOPPED, 'Test 15.4: Door remains in STATE_STOPPED', `got ${getDebugString(global.currentState)}`);
  assert(global.pendingState === STATE_UNKNOWN, 'Test 15.5: pendingState reset', `got ${global.pendingState}`);
  assert(global.pendingPulsesCount === 0, 'Test 15.6: pendingPulsesCount reset to 0', `got ${global.pendingPulsesCount}`);
})();

// Test 16: Leaving sensor grace period when starting movement
(() => {
  resetEnvironment();
  global.currentState = STATE_OPENING;
  global.previousState = STATE_CLOSED;
  global.lastStateChangeTime = millis();
  global.realClosedSensor = true; // Magnet still resting on switch

  updateLogic();
  assert(global.currentState === STATE_OPENING, 'Test 16.1: updateLogic() preserves OPENING when starting from CLOSED sensor', `got ${getDebugString(global.currentState)}`);

  // Same for OPEN sensor when closing from OPEN
  resetEnvironment();
  global.currentState = STATE_CLOSING;
  global.previousState = STATE_OPEN;
  global.lastStateChangeTime = millis();
  global.realOpenSensor = true; // Magnet still resting on switch

  updateLogic();
  assert(global.currentState === STATE_CLOSING, 'Test 16.2: updateLogic() preserves CLOSING when starting from OPEN sensor', `got ${getDebugString(global.currentState)}`);
})();

// Test 17: Concurrent request rejected with 429 during active pending sequence
(() => {
  resetEnvironment();
  global.currentState = STATE_STOPPED;
  global.previousState = STATE_OPENING;

  handleOn();
  handleOff(); // Attempt opposite command while sequence is pending
  assert(lastServerResponse && lastServerResponse.code === 429, 'Test 17.1: Concurrent command returns 429 while sequence is pending', `got ${lastServerResponse ? lastServerResponse.code : 'none'}`);
})();

// --- REBOOT EVALUATION TEST SUITE FOR ALL STATES ---

// Reboot Test 1: Reboot at Fully Closed Position (0%)
(() => {
  resetEnvironment();
  global.pinClosedSensor = 0; // CLOSED switch active
  global.pinOpenSensor = 1;
  global.nvsStore = { curr: STATE_CLOSED, prev: STATE_CLOSED, s_close: true, s_open: false };

  setup();
  assert(global.currentState === STATE_CLOSED, 'Reboot 1.1: Reboot at CLOSED limit switch sets currentState to CLOSED', `got ${getDebugString(global.currentState)}`);
  assert(global.previousState === STATE_CLOSED, 'Reboot 1.2: Reboot at CLOSED limit switch sets previousState to CLOSED', `got ${getDebugString(global.previousState)}`);
})();

// Reboot Test 2: Reboot at Fully Open Position (100%)
(() => {
  resetEnvironment();
  global.pinClosedSensor = 1;
  global.pinOpenSensor = 0; // OPEN switch active
  global.nvsStore = { curr: STATE_OPEN, prev: STATE_OPEN, s_close: false, s_open: true };

  setup();
  assert(global.currentState === STATE_OPEN, 'Reboot 2.1: Reboot at OPEN limit switch sets currentState to OPEN', `got ${getDebugString(global.currentState)}`);
  assert(global.previousState === STATE_OPEN, 'Reboot 2.2: Reboot at OPEN limit switch sets previousState to OPEN', `got ${getDebugString(global.previousState)}`);
})();

// Reboot Test 3: Reboot Mid-Track after STATE_OPENING pre-power loss
(() => {
  resetEnvironment();
  global.pinClosedSensor = 1; // Mid-track (neither sensor active)
  global.pinOpenSensor = 1;
  global.nvsStore = { curr: STATE_OPENING, prev: STATE_CLOSED, s_close: false, s_open: false };

  setup();
  assert(global.currentState === STATE_STOPPED, 'Reboot 3.1: Reboot mid-track sets motor currentState to STOPPED', `got ${getDebugString(global.currentState)}`);
  assert(global.previousState === STATE_OPENING, 'Reboot 3.2: Reboot mid-track recovers pre-power loss direction -> WAS OPENING', `got ${getDebugString(global.previousState)}`);
})();

// Reboot Test 4: Reboot Mid-Track after STATE_CLOSING pre-power loss
(() => {
  resetEnvironment();
  global.pinClosedSensor = 1; // Mid-track (neither sensor active)
  global.pinOpenSensor = 1;
  global.nvsStore = { curr: STATE_CLOSING, prev: STATE_OPEN, s_close: false, s_open: false };

  setup();
  assert(global.currentState === STATE_STOPPED, 'Reboot 4.1: Reboot mid-track sets motor currentState to STOPPED', `got ${getDebugString(global.currentState)}`);
  assert(global.previousState === STATE_CLOSING, 'Reboot 4.2: Reboot mid-track recovers pre-power loss direction -> WAS CLOSING', `got ${getDebugString(global.previousState)}`);
})();

// Reboot Test 5: Reboot Mid-Track after STATE_STOPPED pre-power loss
(() => {
  resetEnvironment();
  global.pinClosedSensor = 1;
  global.pinOpenSensor = 1;
  global.nvsStore = { curr: STATE_STOPPED, prev: STATE_CLOSING, s_close: false, s_open: false };

  setup();
  assert(global.currentState === STATE_STOPPED, 'Reboot 5.1: Reboot mid-track sets motor currentState to STOPPED', `got ${getDebugString(global.currentState)}`);
  assert(global.previousState === STATE_CLOSING, 'Reboot 5.2: Reboot mid-track preserves previousState direction', `got ${getDebugString(global.previousState)}`);
})();

// Reboot Test 6: Reboot Mid-Track on Fresh ESP32 Flash (Zero NVS History)
(() => {
  resetEnvironment();
  global.pinClosedSensor = 1;
  global.pinOpenSensor = 1;
  global.nvsStore = {}; // Empty NVS

  setup();
  assert(global.currentState === STATE_STOPPED, 'Reboot 6.1: Reboot on fresh flash sets currentState to STOPPED', `got ${getDebugString(global.currentState)}`);
  assert(global.previousState === STATE_UNKNOWN, 'Reboot 6.2: Reboot on fresh flash sets previousState to UNKNOWN', `got ${getDebugString(global.previousState)}`);
})();

// --- NEW TEST SUITE: EXTERNAL MANUAL OVERRIDE & EMERGENCY RELEASE ---
// Test 18: External Manual Override from CLOSED position (sensor unseats without relay pulse)
(() => {
  resetEnvironment();
  global.currentState = STATE_CLOSED;
  global.previousState = STATE_CLOSED;
  global.realClosedSensor = true;
  global.pinClosedSensor = 0;

  // Door physically pulled or opened via manual wall button/remote
  global.realClosedSensor = false;
  global.pinClosedSensor = 1;

  updateLogic();
  assert(global.currentState === STATE_OPENING, 'Test 18.1: Unseating CLOSED sensor without pulse transitions to STATE_OPENING', `got ${getDebugString(global.currentState)}`);
})();

// Test 19: External Manual Override from OPEN position (sensor unseats without relay pulse)
(() => {
  resetEnvironment();
  global.currentState = STATE_OPEN;
  global.previousState = STATE_OPEN;
  global.realOpenSensor = true;
  global.pinOpenSensor = 0;

  // Door physically pulled or closed via manual wall button/remote
  global.realOpenSensor = false;
  global.pinOpenSensor = 1;

  updateLogic();
  assert(global.currentState === STATE_CLOSING, 'Test 19.1: Unseating OPEN sensor without pulse transitions to STATE_CLOSING', `got ${getDebugString(global.currentState)}`);
})();

// --- NEW TEST SUITE: MOTOR OBSTACLE AUTO-REVERSALS & MID-TRACK STALL ---
// Test 20: Auto-Reversal while CLOSING (obstruction trips motor auto-reverse back to OPEN limit switch)
(() => {
  resetEnvironment();
  global.currentState = STATE_CLOSING;
  global.previousState = STATE_OPEN;
  global.lastStateChangeTime = millis() - 5000;
  global.realOpenSensor = true; // Bounced back and triggered OPEN sensor

  updateLogic();
  assert(global.currentState === STATE_OPEN, 'Test 20.1: Door bounces back to OPEN limit switch', `got ${getDebugString(global.currentState)}`);
  assert(global.obstacleWarning === true, 'Test 20.2: OBSTACLE_WARNING flag set on opposite limit switch trip', `got ${global.obstacleWarning}`);
})();

// Test 21: Mid-Track Stall locks out automated commands until manual toggle
(() => {
  resetEnvironment();
  global.currentState = STATE_CLOSING;
  global.previousState = STATE_OPEN;
  global.lastStateChangeTime = millis() - (MOVEMENT_TIMEOUT + 1000); // > MOVEMENT_TIMEOUT

  updateLogic();
  assert(global.currentState === STATE_STOPPED, 'Test 21.1: Movement timeout forces STATE_STOPPED', `got ${getDebugString(global.currentState)}`);
  assert(global.midTrackStall === true, 'Test 21.2: midTrackStall flag set', `got ${global.midTrackStall}`);
  assert(global.sensorTimeoutError === true, 'Test 21.3: sensorTimeoutError flag set', `got ${global.sensorTimeoutError}`);

  // Automated handleOff() should be locked out
  handleOff();
  assert(lastServerResponse && lastServerResponse.code === 409, 'Test 21.4: handleOff() rejected with 409 while stalled', `got ${lastServerResponse ? lastServerResponse.code : 'none'}`);

  // Manual handleToggle() clears stall and executes unconditional relay pulse
  global.lastPulseTime = millis() - 2000; // allow lockout window to pass
  handleToggle();
  assert(global.midTrackStall === false, 'Test 21.5: handleToggle() clears midTrackStall flag', `got ${global.midTrackStall}`);
  assert(lastServerResponse && lastServerResponse.code === 200, 'Test 21.6: handleToggle() executes pulse successfully (200 OK)', `got ${lastServerResponse ? lastServerResponse.code : 'none'}`);
})();

// --- NEW TEST SUITE: PULSE VERIFICATION (FAULT_FAILED_TO_MOVE) ---
// Test 22: Pulse verification marks failedToMove when switch fails to disengage
(() => {
  resetEnvironment();
  global.currentState = STATE_CLOSED;
  global.realClosedSensor = true;

  handleToggle(); // Triggers relay from CLOSED
  assert(global.pulseVerificationPending === true, 'Test 22.1: pulseVerificationPending set to true', `got ${global.pulseVerificationPending}`);

  // 2.6 seconds pass, switch still engaged (motor locked or power disconnected)
  global.lastPulseTime = millis() - 2600;
  handleRelay();

  assert(global.failedToMove === true, 'Test 22.2: failedToMove flag set when switch fails to disengage', `got ${global.failedToMove}`);
  assert(global.currentState === STATE_STOPPED, 'Test 22.3: Door forced to STATE_STOPPED on failed movement', `got ${getDebugString(global.currentState)}`);
})();

// --- NEW TEST SUITE: NVS FLASH WEAR MINIMIZATION ---
// Test 23: NVS writes are skipped for transient moving states
(() => {
  resetEnvironment();
  global.currentState = STATE_OPENING;
  global.nvsStore = { curr: STATE_CLOSED, prev: STATE_CLOSED, s_close: true, s_open: false };

  // Call saveStateToNVS() directly
  saveStateToNVS();
  assert(global.nvsStore.curr === STATE_CLOSED, 'Test 23.1: NVS curr NOT updated to STATE_OPENING (wear minimization)', `got ${global.nvsStore.curr}`);

  // Transitions to resting STATE_OPEN
  global.currentState = STATE_OPEN;
  saveStateToNVS();
  assert(global.nvsStore.curr === STATE_OPEN, 'Test 23.2: NVS curr updated on stable resting STATE_OPEN', `got ${global.nvsStore.curr}`);
})();

// --- NEW TEST SUITE: FALLBACK DUMB BUTTON CYCLING ---
// Test 24: When limit switches fail/unresponsive (severed wires, both inactive), toggle cycles Opening -> Stopped -> Closing -> Stopped
(() => {
  resetEnvironment();
  global.realClosedSensor = false;
  global.realOpenSensor = false; // Cut wires / severed sensors: both inactive
  global.currentState = STATE_STOPPED;
  global.previousState = STATE_UNKNOWN;
  global.lastCommandedDirection = STATE_UNKNOWN;
  global.lastPulseTime = millis() - 2000;

  // Toggle 1: Starts Opening
  handleToggle();
  assert(global.currentState === STATE_OPENING, 'Test 24.1: Toggle 1 starts dumb OPENING cycle', `got ${getDebugString(global.currentState)}`);

  // Relay releases after 200ms
  global.relayActive = false;
  global.lastPulseTime = millis() - 2000;

  // Toggle 2: Stops movement mid-track
  handleToggle();
  assert(global.currentState === STATE_STOPPED, 'Test 24.2: Toggle 2 STOPPED dumb cycle', `got ${getDebugString(global.currentState)}`);

  // Relay releases after 200ms
  global.relayActive = false;
  global.lastPulseTime = millis() - 2000;

  // Toggle 3: Starts Closing (Reversed)
  handleToggle();
  assert(global.currentState === STATE_CLOSING, 'Test 24.3: Toggle 3 starts dumb CLOSING cycle (reversed)', `got ${getDebugString(global.currentState)}`);

  // Relay releases after 200ms
  global.relayActive = false;
  global.lastPulseTime = millis() - 2000;

  // Toggle 4: Stops movement again
  handleToggle();
  assert(global.currentState === STATE_STOPPED, 'Test 24.4: Toggle 4 STOPPED dumb cycle again', `got ${getDebugString(global.currentState)}`);
})();



// --- NEW TEST SUITE: RAPID COMMAND FLOOD LOCKOUT ---
// Test 25: Commands within 1.5s lockout are rejected
(() => {
  resetEnvironment();
  global.currentState = STATE_CLOSED;
  global.realClosedSensor = true;
  global.lastPulseTime = millis(); // Pulse just happened 50ms ago

  handleToggle();
  assert(lastServerResponse && lastServerResponse.code === 429, 'Test 25.1: handleToggle() rejects rapid flood with 429', `got ${lastServerResponse ? lastServerResponse.code : 'none'}`);

  handleOn();
  assert(lastServerResponse && lastServerResponse.code === 429, 'Test 25.2: handleOn() rejects rapid flood with 429', `got ${lastServerResponse ? lastServerResponse.code : 'none'}`);
})();

// --- NEW TEST SUITE: POSITION TRACKING & FLIGHT PROGRESSION ---
// Test 26: calculateCurrentPosition() interpolates from relay-initiated flight start time
(() => {
  resetEnvironment();
  global.currentState = STATE_OPENING;
  global.openDurationMs = 17000;
  global.startPositionPct = 0;
  global.activeFlightStartTime = millis();

  // 26.1: Opening: Starts at 0% when relay fires
  assert(calculateCurrentPosition() === 0, 'Test 26.1: Opening position starts at 0% upon relay trigger', `got ${calculateCurrentPosition()}`);

  // 26.2: Advances halfway through flight (8500ms / 17000ms = 50%)
  global.activeFlightStartTime = millis() - 8500;
  assert(calculateCurrentPosition() === 50, 'Test 26.2: Opening position advances to 50% halfway through flight', `got ${calculateCurrentPosition()}`);

  // 26.3: Closing: Starts at 100% when relay fires
  global.currentState = STATE_CLOSING;
  global.closeDurationMs = 17000;
  global.startPositionPct = 100;
  global.activeFlightStartTime = millis();
  assert(calculateCurrentPosition() === 100, 'Test 26.3: Closing position starts at 100% upon relay trigger', `got ${calculateCurrentPosition()}`);

  // 26.4: Closing: Halfway through flight (8500ms / 17000ms = 50%)
  global.activeFlightStartTime = millis() - 8500;
  assert(calculateCurrentPosition() === 50, 'Test 26.4: Closing position reduces to 50% halfway through flight', `got ${calculateCurrentPosition()}`);

  // 26.5: Stopped: Returns latched currentPositionPct
  global.currentState = STATE_STOPPED;
  global.currentPositionPct = 42;
  assert(calculateCurrentPosition() === 42, 'Test 26.5: Stopped door returns latched currentPositionPct', `got ${calculateCurrentPosition()}`);
})();

// --- NEW TEST SUITE: PASSIVE AUTO-CALIBRATION ---
// Test 27: Passive calibration runs on complete natural strokes
(() => {
  resetEnvironment();
  const now = 200000;
  global.simulatedMillis = now;
  global.currentState = STATE_OPENING;
  global.previousState = STATE_CLOSED;
  global.openDurationMs = 17000;
  global.switchUnseated = true;
  global.hasIntermediateStop = false;
  global.activeFlightStartTime = now - 17000; // 17.0s flight to switch (+1s activation compensation = 18.0s)
  global.isCalibrated = false;

  // Door completes flight and triggers OPEN sensor
  global.realOpenSensor = true;
  global.realClosedSensor = false;
  updateLogic();

  // (17000 * 3 + 18000) / 4 = 69000 / 4 = 17250
  assert(global.isCalibrated === true, 'Test 27.1: Full stroke sets isCalibrated to true', `got ${global.isCalibrated}`);
  assert(global.openDurationMs === 17250, 'Test 27.2: openDurationMs smoothed via EMA (17250ms)', `got ${global.openDurationMs}`);
  assert(global.lastCalibrationTimeOpen > 0, 'Test 27.3: lastCalibrationTimeOpen recorded', `got ${global.lastCalibrationTimeOpen}`);

  // 27.4: Daily Lockout: Second stroke in the same day does not recalibrate
  let previousOpenMs = global.openDurationMs;
  global.currentState = STATE_OPENING;
  global.previousState = STATE_CLOSED;
  global.switchUnseated = true;
  global.hasIntermediateStop = false;
  global.activeFlightStartTime = now - 19000;
  global.realOpenSensor = true;
  updateLogic();
  assert(global.openDurationMs === previousOpenMs, 'Test 27.4: Daily lockout prevents second calibration within 24h', `got ${global.openDurationMs}`);

  // 27.5: Independent Closing Calibration
  global.currentState = STATE_CLOSING;
  global.previousState = STATE_OPEN;
  global.closeDurationMs = 17000;
  global.switchUnseated = true;
  global.hasIntermediateStop = false;
  global.activeFlightStartTime = now - 18000; // 18.0s flight to switch (+1s activation compensation = 19.0s)
  global.realOpenSensor = false;
  global.realClosedSensor = true; // hits closed switch
  updateLogic();
  // (17000 * 3 + 19000) / 4 = 70000 / 4 = 17500
  assert(global.closeDurationMs === 17500, 'Test 27.5: Closing stroke independently calibrates closeDurationMs', `got ${global.closeDurationMs}`);
  delete global.simulatedMillis;
})();

// --- NEW TEST SUITE: OUTLIER & MANUAL PAUSE REJECTION ---
// Test 28: Rejects flights outside plausible window (<15s, >25s) or deviation > 3.5s
(() => {
  resetEnvironment();
  global.openDurationMs = 17000;
  global.isCalibrated = true;
  global.lastCalibrationTimeOpen = 0; // ready for daily calibration

  // 28.1: Too short (< 15.0s, e.g. 13.0s raw + 1.0s = 14.0s)
  global.currentState = STATE_OPENING;
  global.previousState = STATE_CLOSED;
  global.switchUnseated = true;
  global.hasIntermediateStop = false;
  global.activeFlightStartTime = millis() - 13000;
  global.realOpenSensor = true;
  updateLogic();
  assert(global.openDurationMs === 17000, 'Test 28.1: Flight < 15s discarded from calibration', `got ${global.openDurationMs}`);

  // 28.2: Too long (> 25.0s, e.g. 26.0s)
  resetEnvironment();
  global.openDurationMs = 17000;
  global.isCalibrated = true;
  global.lastCalibrationTimeOpen = 0;
  global.currentState = STATE_OPENING;
  global.previousState = STATE_CLOSED;
  global.switchUnseated = true;
  global.hasIntermediateStop = false;
  global.activeFlightStartTime = millis() - 26000;
  global.realOpenSensor = true;
  updateLogic();
  assert(global.openDurationMs === 17000, 'Test 28.2: Flight > 25s discarded from calibration', `got ${global.openDurationMs}`);

  // 28.3: Deviation > 3.5s from baseline (e.g. 21.0s vs 17.0s baseline = 4.0s deviation)
  resetEnvironment();
  global.openDurationMs = 17000;
  global.isCalibrated = true;
  global.lastCalibrationTimeOpen = 0;
  global.currentState = STATE_OPENING;
  global.previousState = STATE_CLOSED;
  global.switchUnseated = true;
  global.hasIntermediateStop = false;
  global.activeFlightStartTime = millis() - 21000;
  global.realOpenSensor = true;
  updateLogic();
  assert(global.openDurationMs === 17000, 'Test 28.3: Flight with >3.5s deviation from baseline discarded (manual pause)', `got ${global.openDurationMs}`);

  // 28.4: Intermediate stop flag prevents calibration
  resetEnvironment();
  global.openDurationMs = 17000;
  global.isCalibrated = false;
  global.currentState = STATE_OPENING;
  global.previousState = STATE_CLOSED;
  global.switchUnseated = true;
  global.hasIntermediateStop = true; // had manual stop during stroke
  global.activeFlightStartTime = millis() - 18000;
  global.realOpenSensor = true;
  updateLogic();
  assert(global.openDurationMs === 17000, 'Test 28.4: Stroke with intermediate stop discarded', `got ${global.openDurationMs}`);
})();

// --- NEW TEST SUITE: NVS WEAR PROTECTION & COOLDOWN ---
// Test 29: NVS Flash commits only when resting at CLOSED, delta >= 200ms, and cooldown respected
(() => {
  resetEnvironment();
  global.nvsStore = {};
  global.currentState = STATE_CLOSED;
  global.openDurationMs = 17100;
  global.nvsStoredOpenMs = 17000; // delta 100ms (< 200ms)
  global.lastNVSWriteTime = 0;

  saveStateToNVS();
  assert(global.nvsStore["open_ms"] === undefined, 'Test 29.1: Delta < 200ms is NOT committed to NVS', `got ${global.nvsStore["open_ms"]}`);

  // Drift >= 200ms triggers write at STATE_CLOSED
  global.openDurationMs = 17250; // delta 250ms (>= 200ms)
  saveStateToNVS();
  assert(global.nvsStore["open_ms"] === 17250, 'Test 29.2: Cumulative drift >= 200ms is committed to NVS', `got ${global.nvsStore["open_ms"]}`);
  assert(global.nvsStoredOpenMs === 17250, 'Test 29.3: nvsStoredOpenMs updated in RAM', `got ${global.nvsStoredOpenMs}`);

  // Immediate subsequent write is blocked by 5-minute cooldown
  global.openDurationMs = 18000; // another delta > 200ms
  saveStateToNVS();
  assert(global.nvsStore["open_ms"] === 17250, 'Test 29.4: Write blocked during 5-minute cooldown', `got ${global.nvsStore["open_ms"]}`);

  // STATE_STOPPED writes stop_pos
  global.currentState = STATE_STOPPED;
  global.currentPositionPct = 65;
  saveStateToNVS();
  assert(global.nvsStore["stop_pos"] === 65, 'Test 29.5: STATE_STOPPED writes stop_pos to NVS', `got ${global.nvsStore["stop_pos"]}`);
})();

// --- NEW TEST SUITE: CALIBRATE RESET ENDPOINT ---
// Test 30: handleCalibrateReset() resets durations and clears calibration flag safely
(() => {
  resetEnvironment();
  pulseLog = [];
  global.openDurationMs = 19500;
  global.closeDurationMs = 18800;
  global.lastCalibrationTimeOpen = millis();
  global.lastCalibrationTimeClose = millis();

  handleCalibrateReset();

  assert(global.openDurationMs === 17000, 'Test 30.1: handleCalibrateReset() resets openDurationMs to 17000', `got ${global.openDurationMs}`);
  assert(global.closeDurationMs === 17000, 'Test 30.2: handleCalibrateReset() resets closeDurationMs to 17000', `got ${global.closeDurationMs}`);
  assert(global.isCalibrated === false, 'Test 30.3: handleCalibrateReset() resets isCalibrated to false', `got ${global.isCalibrated}`);
  assert(global.lastCalibrationTimeOpen === 0, 'Test 30.4: Clears open daily calibration quota', `got ${global.lastCalibrationTimeOpen}`);
  assert(global.lastCalibrationTimeClose === 0, 'Test 30.5: Clears close daily calibration quota', `got ${global.lastCalibrationTimeClose}`);
  assert(pulseLog.length === 0, 'Test 30.6: Zero relay pulses sent during calibration reset', `got ${pulseLog.length}`);
  assert(lastServerResponse && lastServerResponse.code === 200, 'Test 30.7: Returns HTTP 200 OK', `got ${lastServerResponse ? lastServerResponse.code : 'none'}`);
})();

console.log('\n====================================================');
console.log(`  📊 TEST RESULTS: ${passedTests}/${totalTests} Passed (${failedTests} Failed)`);
console.log('====================================================\n');

if (failedTests > 0) {
  process.exit(1);
} else {
  process.exit(0);
}

