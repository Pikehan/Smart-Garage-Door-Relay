/**
 * Automated test suite for ESP32 OTA and firmware updates.
 */

const crypto = require('crypto');

// Enums & Constants
const STATE_UNKNOWN = 0;
const STATE_CLOSED = 1;
const STATE_OPENING = 2;
const STATE_CLOSING = 3;
const STATE_OPEN = 4;
const STATE_STOPPED = 5;

const UPLOAD_FILE_START = 0;
const UPLOAD_FILE_WRITE = 1;
const UPLOAD_FILE_END = 2;
const UPLOAD_FILE_ABORTED = 3;

const ESP_OTA_IMG_PENDING_VERIFY = 0;
const ESP_OTA_IMG_VALID = 1;
const ESP_OTA_IMG_INVALID = 2;

const PARTITION_MAX_SIZE = 1966080;
const FLASH_SECTOR_SIZE = 4096;
const HEAP_MIN_REQUIRED = 20000;
const RECONNECT_INTERVAL_MS = 10000;

// Structured Assertion Harness with Diff Traces and Call Stacks
let passedTests = 0;
let failedTests = 0;
let totalTests = 0;
const failures = [];

function assertEqual(actual, expected, testName, details = '') {
  totalTests++;
  const actualStr = JSON.stringify(actual);
  const expectedStr = JSON.stringify(expected);
  if (actualStr === expectedStr) {
    console.log(`  ✅ PASS: ${testName}`);
    passedTests++;
    return true;
  } else {
    const err = new Error();
    const stack = (err.stack || '').split('\n').slice(2, 6).join('\n');
    console.error(`  ❌ FAIL: ${testName} ${details ? '(' + details + ')' : ''}`);
    console.error(`     Expected: ${expectedStr}`);
    console.error(`     Actual:   ${actualStr}`);
    console.error(`     Trace:\n${stack}`);
    failedTests++;
    failures.push({ testName, expected, actual, stack });
    return false;
  }
}

function assert(condition, testName, details = '') {
  return assertEqual(Boolean(condition), true, testName, details);
}

// PRNG Helper (Mulberry32)
class Mulberry32 {
  constructor(seed = 0xCAFEBABE) {
    this.seed = seed >>> 0;
  }
  next() {
    let t = (this.seed += 0x6D2B79F5) >>> 0;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  }
  nextInt(min, max) {
    return Math.floor(this.next() * (max - min + 1)) + min;
  }
  randomBytes(len) {
    const buf = Buffer.alloc(len);
    for (let i = 0; i < len; i++) {
      buf[i] = this.nextInt(0, 255);
    }
    return buf;
  }
}

// Unsigned 32-bit modulo time difference helper (mimics uint32_t arithmetic in ESP-IDF)
function timeDiff(now, past) {
  return ((now - past) >>> 0);
}

// Mock Mutex Engine
class MockMutex {
  constructor() {
    this.locked = false;
    this.owner = null;
  }

  acquire(owner, timeoutMs = 500) {
    if (!this.locked) {
      this.locked = true;
      this.owner = owner;
      return true;
    }
    if (this.owner === owner) {
      return true; // Reentrant
    }
    // Timeout during contention
    return false;
  }

  release(owner) {
    if (this.owner === owner) {
      this.locked = false;
      this.owner = null;
      return true;
    }
    return false;
  }
}

class MockUpdate {
  constructor(partitionSize = PARTITION_MAX_SIZE) {
    this.partitionSize = partitionSize;
    this.active = false;
    this.aborted = false;
    this.hasErrorFlag = false;
    this.writtenBytes = 0;
    this.expectedMD5 = null;
    this.hasher = null;
    this.failBegin = false;
    this.failWriteAtChunk = null;
    this.chunkCount = 0;
    this.simulateZeroByteWrite = false;
    this.erasedSectors = new Set();
    this.sectorWrites = new Map();
  }

  begin(size = PARTITION_MAX_SIZE) {
    if (this.failBegin) return false;
    if (size > this.partitionSize) return false;
    this.active = true;
    this.aborted = false;
    this.hasErrorFlag = false;
    this.writtenBytes = 0;
    this.chunkCount = 0;
    this.hasher = crypto.createHash('md5');
    this.erasedSectors.clear();
    this.sectorWrites.clear();
    return true;
  }

  setMD5(md5) {
    const trimmed = (md5 || '').trim();
    // Validate standard 32-character hex format
    if (trimmed.length === 32 && /^[0-9a-fA-F]{32}$/.test(trimmed)) {
      this.expectedMD5 = trimmed.toLowerCase();
      return true;
    }
    this.expectedMD5 = null;
    return false;
  }

  write(buf, len) {
    if (!this.active || this.aborted || this.hasErrorFlag) return 0;
    this.chunkCount++;

    if (this.failWriteAtChunk !== null && this.chunkCount === this.failWriteAtChunk) {
      this.hasErrorFlag = true;
      return 0;
    }

    if (this.simulateZeroByteWrite) {
      return 0;
    }

    // Partition boundary overflow check
    if (this.writtenBytes + len > this.partitionSize) {
      this.hasErrorFlag = true;
      this.aborted = true;
      return 0;
    }

    // 4KB Sector Tracking
    if (len > 0) {
      const startSector = Math.floor(this.writtenBytes / FLASH_SECTOR_SIZE);
      const endSector = Math.floor((this.writtenBytes + len - 1) / FLASH_SECTOR_SIZE);
      for (let s = startSector; s <= endSector; s++) {
        this.erasedSectors.add(s);
        this.sectorWrites.set(s, (this.sectorWrites.get(s) || 0) + len);
      }
    }

    if (this.hasher) {
      this.hasher.update(buf.slice(0, len));
    }
    this.writtenBytes += len;
    return len;
  }

  end(evenIfUnfinished = true) {
    if (!this.active || this.aborted || this.hasErrorFlag) return false;
    if (this.writtenBytes === 0) {
      this.hasErrorFlag = true;
      return false; // Zero bytes written
    }
    if (this.expectedMD5 && this.hasher) {
      const calculatedMD5 = this.hasher.digest('hex').toLowerCase();
      if (calculatedMD5 !== this.expectedMD5) {
        this.hasErrorFlag = true;
        return false;
      }
    }
    this.active = false;
    return true;
  }

  abort() {
    this.active = false;
    this.aborted = true;
    // Interrupted sector writes are discarded
    this.sectorWrites.clear();
  }

  hasError() {
    return this.hasErrorFlag;
  }
}

class ESP32SystemMock {
  constructor() {
    // Door State & Motor
    this.currentState = STATE_CLOSED;
    this.previousState = STATE_UNKNOWN;
    this.safetyTaskPaused = false;
    this.relayActive = false;
    this.relayPulses = 0;
    this.lastPulseTime = 0;
    this.doorMutex = new MockMutex();

    // Memory & Time (unsigned 32-bit clock)
    this.maxAllocHeap = 80000;
    this.simulatedMillis = 1000 >>> 0;

    // Network & Wi-Fi
    this.activeSsidPrimary = "Home-WiFi";
    this.activePassPrimary = "SecretPass";
    this.activeSsidBackup = "";
    this.activePassBackup = "";
    this.wifiConnected = false;
    this.wifiConnectedSince = 0 >>> 0;
    this.apModeActive = false;
    this.apDisablePending = false;
    this.mdnsPending = false;
    this.softApStations = 0;
    this.lastWifiAttempt = 0 >>> 0;

    // Partitions & Boot State
    this.runningPartition = { name: "ota_0", state: ESP_OTA_IMG_PENDING_VERIFY, size: PARTITION_MAX_SIZE };
    this.targetPartition = { name: "ota_1", size: PARTITION_MAX_SIZE };
    this.bootPartition = "ota_0";
    this.rollbackCalled = false;
    this.rollbackCancelled = false;

    // Hardware Update Singleton
    this.Update = new MockUpdate(PARTITION_MAX_SIZE);

    // WebOTA Multipart Handler State
    this.webOtaError = null;
    this.webOtaSuspended = false;
    this.webOtaDone = false;
    this.firstChunkVerified = false;

    // ArduinoOTA State
    this.arduinoOtaActive = false;

    // System Control
    this.restartScheduled = false;

    // Async Event Queue (Simulating FreeRTOS sys_evt task)
    this.eventQueue = [];
  }

  // Task & Safety Helpers
  suspendSafetyTask() {
    this.safetyTaskPaused = true;
    this.relayActive = false;
  }

  resumeSafetyTask() {
    this.safetyTaskPaused = false;
  }

  isFirmwareUpdating() {
    return this.safetyTaskPaused;
  }

  // Origin / Referer Security Validation
  isSameOrigin(headers = {}) {
    const origin = headers['Origin'] || headers['origin'] || '';
    const referer = headers['Referer'] || headers['referer'] || '';
    const host = headers['Host'] || headers['host'] || '192.168.1.33';

    const target = origin || referer;
    if (!target) return true; // Direct non-browser CLI or curl

    let stripped = target;
    if (stripped.startsWith('http://')) stripped = stripped.substring(7);
    else if (stripped.startsWith('https://')) stripped = stripped.substring(8);
    const slashIdx = stripped.indexOf('/');
    let targetHost = (slashIdx !== -1) ? stripped.substring(0, slashIdx) : stripped;

    // Normalize default HTTP port 80 out of target and host
    if (targetHost.endsWith(':80')) targetHost = targetHost.substring(0, targetHost.length - 3);
    let normalizedHost = host;
    if (normalizedHost.endsWith(':80')) normalizedHost = normalizedHost.substring(0, normalizedHost.length - 3);

    // Host must strictly match
    return targetHost.toLowerCase() === normalizedHost.toLowerCase();
  }

  // Wi-Fi Event Queue
  postWiFiEvent(event) {
    this.eventQueue.push(event);
  }

  processEvents() {
    while (this.eventQueue.length > 0) {
      const event = this.eventQueue.shift();
      this.onWiFiEvent(event);
    }
  }

  onWiFiEvent(event) {
    switch (event) {
      case 'ARDUINO_EVENT_WIFI_STA_GOT_IP':
        this.wifiConnected = true;
        this.wifiConnectedSince = this.simulatedMillis >>> 0;
        this.mdnsPending = true;
        if (this.apModeActive) {
          this.apDisablePending = true;
        }
        break;
      case 'ARDUINO_EVENT_WIFI_STA_DISCONNECTED':
        this.wifiConnected = false;
        this.wifiConnectedSince = 0 >>> 0;
        break;
    }
  }

  handleWiFiReconnection() {
    if (this.webOtaSuspended || this.isFirmwareUpdating() || this.arduinoOtaActive) {
      return 'BLOCKED_BY_OTA';
    }
    if (this.apDisablePending && this.wifiConnected) {
      this.apDisablePending = false;
      this.apModeActive = false;
    }
    if (this.mdnsPending && this.wifiConnected) {
      this.mdnsPending = false;
    }
    if (this.wifiConnected) {
      return 'CONNECTED';
    }
    const elapsed = timeDiff(this.simulatedMillis, this.lastWifiAttempt);
    if (!this.wifiConnected && (!this.apModeActive || this.softApStations === 0) &&
        (elapsed >= RECONNECT_INTERVAL_MS)) {
      this.lastWifiAttempt = this.simulatedMillis >>> 0;
      return 'RECONNECT_TRIGGERED';
    }
    return 'IDLE';
  }

  // Partition Rollback Validation
  validateAppRollback() {
    if (this.rollbackCalled || this.rollbackCancelled) return;
    if (this.runningPartition.state === ESP_OTA_IMG_PENDING_VERIFY) {
      const hasCredentials = (this.activeSsidPrimary.length > 0 || this.activeSsidBackup.length > 0);
      if (hasCredentials) {
        if (!this.wifiConnected && this.simulatedMillis > 180000) {
          this.rollbackCalled = true;
          this.runningPartition.state = ESP_OTA_IMG_INVALID;
          this.bootPartition = "ota_prev"; // rollback to previous slot
          this.restartScheduled = true;
          return;
        }
        const connectedDuration = timeDiff(this.simulatedMillis, this.wifiConnectedSince);
        if (!this.wifiConnected || (connectedDuration < 30000)) {
          return;
        }
      } else {
        if (this.simulatedMillis < 30000) return;
      }
      this.rollbackCancelled = true;
      this.runningPartition.state = ESP_OTA_IMG_VALID;
    }
  }

  // WebOTA Upload State Machine
  handleUpdateUpload(status, chunk = Buffer.from([]), headers = {}) {
    if (status === UPLOAD_FILE_START) {
      this.webOtaError = null;
      this.webOtaDone = false;
      this.firstChunkVerified = false;

      if (!this.isSameOrigin(headers)) {
        this.webOtaError = "Cross-origin request forbidden";
        this.Update.abort();
        return { status: 403, error: this.webOtaError };
      }

      if (this.isFirmwareUpdating() || this.arduinoOtaActive) {
        this.webOtaError = "Firmware update already in progress";
        return { status: 409, error: this.webOtaError };
      }

      // Door State Mutex Check
      if (!this.doorMutex.acquire('webOta', 500)) {
        this.webOtaError = "State lock acquisition timeout";
        this.Update.abort();
        return { status: 503, error: this.webOtaError };
      }
      try {
        if (this.currentState === STATE_OPENING || this.currentState === STATE_CLOSING) {
          this.webOtaError = "Door in motion. Update aborted.";
          this.Update.abort();
          return { status: 409, error: this.webOtaError };
        }
      } finally {
        this.doorMutex.release('webOta');
      }

      // Low Heap Check
      if (this.maxAllocHeap < HEAP_MIN_REQUIRED) {
        this.webOtaError = "Low Memory: Reboot before update.";
        this.Update.abort();
        return { status: 503, error: this.webOtaError };
      }

      if (!this.webOtaSuspended) {
        this.suspendSafetyTask();
        this.webOtaSuspended = true;
      }

      if (!this.Update.begin(PARTITION_MAX_SIZE)) {
        this.webOtaError = "Update.begin failed";
        this.resumeSafetyTask();
        this.webOtaSuspended = false;
        return { status: 500, error: this.webOtaError };
      }

      if (headers['x-MD5']) {
        this.Update.setMD5(headers['x-MD5']);
      }
      return { status: 200, error: null };
    }

    if (status === UPLOAD_FILE_WRITE) {
      if (this.webOtaError !== null) return { status: 500, error: this.webOtaError };

      if (!this.firstChunkVerified) {
        if (chunk.length === 0) return { status: 200, error: null };
        if (chunk[0] !== 0xE9) {
          this.webOtaError = "Invalid firmware: missing ESP32 magic byte (0xE9)";
          this.Update.abort();
          if (this.webOtaSuspended) {
            this.resumeSafetyTask();
            this.webOtaSuspended = false;
          }
          return { status: 400, error: this.webOtaError };
        }
        this.firstChunkVerified = true;
      }

      const written = this.Update.write(chunk, chunk.length);
      if (written !== chunk.length) {
        this.webOtaError = "Flash write failed";
        this.Update.abort();
        if (this.webOtaSuspended) {
          this.resumeSafetyTask();
          this.webOtaSuspended = false;
        }
        return { status: 500, error: this.webOtaError };
      }
      return { status: 200, error: null };
    }

    if (status === UPLOAD_FILE_END) {
      if (this.webOtaError !== null) return { status: 500, error: this.webOtaError };
      if (!this.firstChunkVerified) {
        this.webOtaError = "Empty or invalid firmware stream";
        this.Update.abort();
        if (this.webOtaSuspended) {
          this.resumeSafetyTask();
          this.webOtaSuspended = false;
        }
        return { status: 500, error: this.webOtaError };
      }
      if (this.Update.end(true)) {
        this.webOtaDone = true;
        return { status: 200, error: null };
      } else {
        this.webOtaError = "Flash write or checksum verification failed";
        if (this.webOtaSuspended) {
          this.resumeSafetyTask();
          this.webOtaSuspended = false;
        }
        return { status: 500, error: this.webOtaError };
      }
    }

    if (status === UPLOAD_FILE_ABORTED) {
      this.Update.abort();
      if (this.webOtaSuspended) {
        this.resumeSafetyTask();
        this.webOtaSuspended = false;
      }
      this.webOtaDone = false;
      return { status: 400, error: "Upload aborted by client" };
    }
  }

  handleUpdatePostComplete(headers = {}) {
    if (!this.isSameOrigin(headers)) {
      if (this.webOtaSuspended) {
        this.resumeSafetyTask();
        this.webOtaSuspended = false;
      }
      this.webOtaError = null;
      this.webOtaDone = false;
      return { status: 403, message: "Cross-origin request forbidden" };
    }

    const hasError = !this.webOtaDone || this.Update.hasError() || (this.webOtaError !== null);
    if (hasError) {
      if (this.webOtaSuspended) {
        this.resumeSafetyTask();
        this.webOtaSuspended = false;
      }
      const errMsg = this.webOtaError || (!this.webOtaDone ? "No firmware data received or upload aborted" : "Firmware update failed!");
      this.webOtaError = null;
      this.webOtaDone = false;
      return { status: 500, message: errMsg };
    } else {
      this.webOtaError = null;
      this.webOtaDone = false;
      this.bootPartition = this.targetPartition.name;
      this.restartScheduled = true;
      return { status: 200, message: "Firmware updated successfully! Rebooting ESP32..." };
    }
  }

  // ArduinoOTA Lifecycle
  handleOtaServiceCheck() {
    if (this.webOtaSuspended || (!this.arduinoOtaActive && this.isFirmwareUpdating())) {
      return false; // blocked
    }
    if (this.arduinoOtaActive || (this.currentState !== STATE_OPENING && this.currentState !== STATE_CLOSING)) {
      return true;
    }
    return false;
  }

  arduinoOtaStart() {
    if (this.webOtaSuspended || (!this.arduinoOtaActive && this.isFirmwareUpdating())) {
      return { status: 409, message: "Firmware update already in progress" };
    }
    if (this.currentState === STATE_OPENING || this.currentState === STATE_CLOSING) {
      return { status: 409, message: "Door is in motion" };
    }
    this.arduinoOtaActive = true;
    this.suspendSafetyTask();
    this.Update.begin(PARTITION_MAX_SIZE);
    return { status: 200, message: "ArduinoOTA started" };
  }

  arduinoOtaEnd() {
    if (this.Update.end(true)) {
      this.arduinoOtaActive = false;
      this.bootPartition = this.targetPartition.name;
      this.restartScheduled = true;
      return { status: 200, message: "ArduinoOTA complete" };
    } else {
      this.arduinoOtaError();
      return { status: 500, message: "ArduinoOTA verification failed" };
    }
  }

  arduinoOtaError() {
    if (this.arduinoOtaActive) {
      this.Update.abort();
      this.arduinoOtaActive = false;
      this.resumeSafetyTask();
    }
  }

  // REST Control Endpoints
  handleToggle() {
    if (this.isFirmwareUpdating()) return { status: 409, message: "Firmware update in progress. Controls locked." };
    this.relayPulses++;
    this.lastPulseTime = this.simulatedMillis >>> 0;
    return { status: 200, message: "Pulse sent" };
  }

  handleOn() {
    if (this.isFirmwareUpdating()) return { status: 409, message: "Firmware update in progress. Controls locked." };
    this.relayPulses++;
    this.lastPulseTime = this.simulatedMillis >>> 0;
    return { status: 200, message: "Door command accepted" };
  }

  handleOff() {
    if (this.isFirmwareUpdating()) return { status: 409, message: "Firmware update in progress. Controls locked." };
    this.relayPulses++;
    this.lastPulseTime = this.simulatedMillis >>> 0;
    return { status: 200, message: "Door command accepted" };
  }

  handleReboot() {
    if (this.isFirmwareUpdating()) return { status: 409, message: "Firmware update in progress. Reboot locked." };
    if (!this.doorMutex.acquire('reboot', 500)) return { status: 503, message: "Door controller busy" };
    try {
      if (this.currentState === STATE_OPENING || this.currentState === STATE_CLOSING) {
        return { status: 409, message: "Door is in motion. Reboot locked for safety." };
      }
      this.suspendSafetyTask();
      this.restartScheduled = true;
      return { status: 200, message: "Rebooting ESP32 controller..." };
    } finally {
      this.doorMutex.release('reboot');
    }
  }

  handleSetupSave(ssid, pass) {
    if ((ssid || '').length === 0) return { status: 400, message: "Primary SSID cannot be empty." };
    if (this.isFirmwareUpdating()) return { status: 409, message: "Firmware update in progress. Setup locked." };
    if (!this.doorMutex.acquire('setup', 500)) return { status: 503, message: "Door controller busy" };
    try {
      if (this.currentState === STATE_OPENING || this.currentState === STATE_CLOSING) {
        return { status: 409, message: "Door is in motion. Setup locked for safety." };
      }
      this.suspendSafetyTask();
      this.activeSsidPrimary = ssid;
      this.activePassPrimary = pass;
      this.restartScheduled = true;
      return { status: 200, message: "Wi-Fi credentials saved" };
    } finally {
      this.doorMutex.release('setup');
    }
  }

  handleCalibrateReset() {
    if (this.isFirmwareUpdating()) return { status: 409, message: "Firmware update in progress. Calibration locked." };
    if (!this.doorMutex.acquire('calibrate', 500)) return { status: 503, message: "Door controller busy" };
    try {
      if (this.currentState === STATE_OPENING || this.currentState === STATE_CLOSING) {
        return { status: 409, message: "Door is in motion. Calibration locked for safety." };
      }
      return { status: 200, message: "Calibration reset" };
    } finally {
      this.doorMutex.release('calibrate');
    }
  }
}

// Global System Invariant Verification Hook
function verifySystemInvariants(env, testName) {
  // 1. Relay is NEVER left energized
  if (env.relayActive) {
    throw new Error(`[INVARIANT BREACH] "${testName}": Relay left actively energized!`);
  }
  // 2. Door state mutex is NEVER left orphaned/locked
  if (env.doorMutex.locked) {
    throw new Error(`[INVARIANT BREACH] "${testName}": Door mutex left locked by "${env.doorMutex.owner}"!`);
  }
  // 3. Boot partition is ALWAYS a valid partition identifier
  if (!['ota_0', 'ota_1', 'ota_prev'].includes(env.bootPartition)) {
    throw new Error(`[INVARIANT BREACH] "${testName}": Invalid boot partition target: "${env.bootPartition}"!`);
  }
  // 4. Memory heap is NEVER negative
  if (env.maxAllocHeap < 0) {
    throw new Error(`[INVARIANT BREACH] "${testName}": Negative heap memory reported: ${env.maxAllocHeap}!`);
  }
}

// Sandboxed Test Runner Lifecycle
let currentEnv = null;

function test(name, fn) {
  currentEnv = new ESP32SystemMock();
  try {
    fn(currentEnv);
    verifySystemInvariants(currentEnv, name);
  } catch (err) {
    console.error(`  ❌ UNCAUGHT EXCEPTION in "${name}":`, err.message || err);
    failedTests++;
    totalTests++;
  }
}

console.log('====================================================');
console.log('  🛡️ ESP32 OTA HARDENING TEST SUITE');
console.log('====================================================\n');

console.log('--- Suite 1: Test Sandbox & Invariant Verification ---');

test('Harness 1.1: State sandbox guarantees clean isolation between tests', (env) => {
  env.currentState = STATE_OPEN;
  env.maxAllocHeap = 10000;
  env.wifiConnected = true;
  assertEqual(env.currentState, STATE_OPEN, 'Harness 1.1a: State mutates in local test');
  assertEqual(env.maxAllocHeap, 10000, 'Harness 1.1b: Heap mutates in local test');
});

test('Harness 1.2: Subsequent test receives freshly instantiated sandbox instance', (env) => {
  assertEqual(env.currentState, STATE_CLOSED, 'Harness 1.2a: State cleanly reset to default (CLOSED)');
  assertEqual(env.maxAllocHeap, 80000, 'Harness 1.2b: Heap cleanly reset to default (80000)');
  assertEqual(env.wifiConnected, false, 'Harness 1.2c: Wi-Fi status cleanly reset to default (false)');
});

console.log('\n--- Suite 2: Cryptographic MD5 & Stream Verification ---');

test('Crypto 2.1: Valid binary with matching MD5 checksum succeeds', (env) => {
  const binaryPayload = Buffer.concat([
    Buffer.from([0xE9, 0x04, 0x02, 0x10]),
    crypto.randomBytes(4096)
  ]);
  const expectedMD5 = crypto.createHash('md5').update(binaryPayload).digest('hex');

  const startRes = env.handleUpdateUpload(UPLOAD_FILE_START, null, { 'x-MD5': expectedMD5 });
  assertEqual(startRes.status, 200, 'Crypto 2.1a: UPLOAD_FILE_START accepts valid session');

  const writeRes = env.handleUpdateUpload(UPLOAD_FILE_WRITE, binaryPayload);
  assertEqual(writeRes.status, 200, 'Crypto 2.1b: Binary payload written to flash');

  const endRes = env.handleUpdateUpload(UPLOAD_FILE_END);
  assertEqual(endRes.status, 200, 'Crypto 2.1c: UPLOAD_FILE_END succeeds on MD5 match');

  const postRes = env.handleUpdatePostComplete();
  assertEqual(postRes.status, 200, 'Crypto 2.1d: Post completion returns HTTP 200');
  assertEqual(env.bootPartition, "ota_1", 'Crypto 2.1e: Boot partition updated to target slot');
});

test('Crypto 2.2: Corrupted payload with mismatched MD5 checksum is rejected', (env) => {
  const binaryPayload = Buffer.concat([
    Buffer.from([0xE9, 0x04, 0x02, 0x10]),
    crypto.randomBytes(2048)
  ]);
  const falseMD5 = "0123456789abcdef0123456789abcdef";

  env.handleUpdateUpload(UPLOAD_FILE_START, null, { 'x-MD5': falseMD5 });
  env.handleUpdateUpload(UPLOAD_FILE_WRITE, binaryPayload);
  const endRes = env.handleUpdateUpload(UPLOAD_FILE_END);

  assertEqual(endRes.status, 500, 'Crypto 2.2a: UPLOAD_FILE_END fails on mismatched MD5');
  assertEqual(env.Update.hasError(), true, 'Crypto 2.2b: Update singleton marked hasError');
  assertEqual(env.safetyTaskPaused, false, 'Crypto 2.2c: Safety task resumed on checksum failure');

  const postRes = env.handleUpdatePostComplete();
  assertEqual(postRes.status, 500, 'Crypto 2.2d: HTTP 500 returned on corrupted upload');
  assertEqual(env.bootPartition, "ota_0", 'Crypto 2.2e: Boot partition remains untouched');
});

test('Crypto 2.3: Multipart boundary fuzzing across fragmented chunk sizes', (env) => {
  const payload = Buffer.concat([
    Buffer.from([0xE9, 0x08, 0x02, 0x10]),
    crypto.randomBytes(8188)
  ]);
  const validMD5 = crypto.createHash('md5').update(payload).digest('hex');

  env.handleUpdateUpload(UPLOAD_FILE_START, null, { 'x-MD5': validMD5 });

  const chunkSizes = [0, 0, 1, 7, 0, 13, 1024, 64, 7083];
  let offset = 0;

  for (let i = 0; i < chunkSizes.length; i++) {
    const size = chunkSizes[i];
    const chunk = (size === 0) ? Buffer.from([]) : payload.slice(offset, offset + size);
    if (size > 0) offset += size;

    const res = env.handleUpdateUpload(UPLOAD_FILE_WRITE, chunk);
    assertEqual(res.status, 200, `Crypto 2.3 chunk ${i} (${size}b): Written without error`);
  }

  assertEqual(offset, payload.length, 'Crypto 2.3: All payload bytes accounted for');
  assertEqual(env.firstChunkVerified, true, 'Crypto 2.3: firstChunkVerified set after zero-length chunks');

  const endRes = env.handleUpdateUpload(UPLOAD_FILE_END);
  assertEqual(endRes.status, 200, 'Crypto 2.3: UPLOAD_FILE_END passed on fuzzed stream');
  assertEqual(env.Update.writtenBytes, payload.length, 'Crypto 2.3: Exact byte count written to flash');
});

test('Crypto 2.4: Fuzzed stream with bad magic byte after zero-length chunks is rejected', (env) => {
  env.handleUpdateUpload(UPLOAD_FILE_START);

  env.handleUpdateUpload(UPLOAD_FILE_WRITE, Buffer.from([]));
  assertEqual(env.firstChunkVerified, false, 'Crypto 2.4a: Zero chunk does not verify magic byte');

  env.handleUpdateUpload(UPLOAD_FILE_WRITE, Buffer.from([]));
  assertEqual(env.firstChunkVerified, false, 'Crypto 2.4b: Second zero chunk does not verify');

  const badChunk = Buffer.from([0x48, 0x54, 0x54, 0x50]);
  const res = env.handleUpdateUpload(UPLOAD_FILE_WRITE, badChunk);

  assertEqual(res.status, 400, 'Crypto 2.4c: Bad magic byte returns HTTP 400');
  assertEqual(env.Update.aborted, true, 'Crypto 2.4d: Update aborted');
  assertEqual(env.safetyTaskPaused, false, 'Crypto 2.4e: Safety task unpaused');
  assertEqual(env.Update.writtenBytes, 0, 'Crypto 2.4f: Zero bytes written to flash');
});

test('Crypto 2.5: Partition boundary exceeded cleanly aborts without flash overflow', (env) => {
  env.handleUpdateUpload(UPLOAD_FILE_START);

  const validHeader = Buffer.from([0xE9, 0x01, 0x02, 0x03]);
  const baseChunk = Buffer.concat([validHeader, crypto.randomBytes(PARTITION_MAX_SIZE - 4)]);
  const res1 = env.handleUpdateUpload(UPLOAD_FILE_WRITE, baseChunk);
  assertEqual(res1.status, 200, 'Crypto 2.5a: Max partition capacity written');

  const overflowChunk = crypto.randomBytes(100);
  const res2 = env.handleUpdateUpload(UPLOAD_FILE_WRITE, overflowChunk);

  assertEqual(res2.status, 500, 'Crypto 2.5b: Overflow chunk returns HTTP 500');
  assertEqual(env.Update.aborted, true, 'Crypto 2.5c: Flash update aborted');
  assertEqual(env.Update.writtenBytes, PARTITION_MAX_SIZE, 'Crypto 2.5d: Flash writes clamped at limit');
  assertEqual(env.safetyTaskPaused, false, 'Crypto 2.5e: Safety task safely restored');
});

console.log('\n--- Suite 3: Dual-Core Concurrency & Mutex Contention ---');

test('Concurrency 3.1: Async Wi-Fi event queue dispatches in separate tick', (env) => {
  env.simulatedMillis = 5000;
  env.postWiFiEvent('ARDUINO_EVENT_WIFI_STA_GOT_IP');

  assertEqual(env.wifiConnected, false, 'Concurrency 3.1a: Flag unchanged before event processing');
  assertEqual(env.wifiConnectedSince, 0, 'Concurrency 3.1b: Timestamp 0 before processing');

  env.processEvents();

  assertEqual(env.wifiConnected, true, 'Concurrency 3.1c: wifiConnected set after sys_evt dispatch');
  assertEqual(env.wifiConnectedSince, 5000, 'Concurrency 3.1d: wifiConnectedSince recorded accurately');
  assertEqual(env.mdnsPending, true, 'Concurrency 3.1e: mdnsPending flagged');
});

test('Concurrency 3.2: Mutex contention timeout when doorSafetyTask holds lock', (env) => {
  const acquired = env.doorMutex.acquire('doorSafetyTask');
  assertEqual(acquired, true, 'Concurrency 3.2a: Core 0 acquires doorStateMutex');

  const otaRes = env.handleUpdateUpload(UPLOAD_FILE_START);

  assertEqual(otaRes.status, 503, 'Concurrency 3.2b: WebOTA rejected with 503 during lock contention');
  assertEqual(otaRes.error, 'State lock acquisition timeout', 'Concurrency 3.2c: Diagnostic message matches');
  assertEqual(env.Update.aborted, true, 'Concurrency 3.2d: Flash update aborted');
  assertEqual(env.safetyTaskPaused, false, 'Concurrency 3.2e: Safety task remains running');

  env.doorMutex.release('doorSafetyTask');
  assertEqual(env.doorMutex.locked, false, 'Concurrency 3.2f: Mutex safely unlocked');
});

test('Concurrency 3.3: Preemption contact trips door into motion right before lock', (env) => {
  env.currentState = STATE_STOPPED;
  env.currentState = STATE_OPENING;

  const otaRes = env.handleUpdateUpload(UPLOAD_FILE_START);
  assertEqual(otaRes.status, 409, 'Concurrency 3.3a: Update rejected with 409 Conflict');
  assertEqual(otaRes.error, 'Door in motion. Update aborted.', 'Concurrency 3.3b: Rejection error message');
  assertEqual(env.Update.active, false, 'Concurrency 3.3c: Update flash session never activated');
  assertEqual(env.safetyTaskPaused, false, 'Concurrency 3.3d: Safety task remains active');
});

console.log('\n--- Suite 4: Hardware & Low-Level Fault Injection ---');

test('Fault 4.1: Low heap allocation rejects update with 503', (env) => {
  env.maxAllocHeap = 18450;

  const otaRes = env.handleUpdateUpload(UPLOAD_FILE_START);
  assertEqual(otaRes.status, 503, 'Fault 4.1a: Rejected with HTTP 503');
  assertEqual(otaRes.error, 'Low Memory: Reboot before update.', 'Fault 4.1b: Low memory diagnostic');
  assertEqual(env.Update.active, false, 'Fault 4.1c: Flash allocation never initialized');
  assertEqual(env.safetyTaskPaused, false, 'Fault 4.1d: Safety task not paused');
});

test('Fault 4.2: Mid-stream SPI flash timeout/failure aborts cleanly', (env) => {
  env.Update.failWriteAtChunk = 2;

  env.handleUpdateUpload(UPLOAD_FILE_START);

  const chunk1 = Buffer.concat([Buffer.from([0xE9, 0x01]), crypto.randomBytes(512)]);
  const res1 = env.handleUpdateUpload(UPLOAD_FILE_WRITE, chunk1);
  assertEqual(res1.status, 200, 'Fault 4.2a: Chunk 1 written successfully');

  const chunk2 = crypto.randomBytes(512);
  const res2 = env.handleUpdateUpload(UPLOAD_FILE_WRITE, chunk2);
  assertEqual(res2.status, 500, 'Fault 4.2b: Chunk 2 write failure returns HTTP 500');
  assertEqual(res2.error, 'Flash write failed', 'Fault 4.2c: Diagnostic error populated');
  assertEqual(env.Update.aborted, true, 'Fault 4.2d: Flash update aborted');
  assertEqual(env.safetyTaskPaused, false, 'Fault 4.2e: Safety task safely resumed');
});

test('Fault 4.3: Aborted update never advances boot partition pointer', (env) => {
  assertEqual(env.bootPartition, "ota_0", 'Fault 4.3a: Initial boot partition is ota_0');

  env.handleUpdateUpload(UPLOAD_FILE_START);
  env.handleUpdateUpload(UPLOAD_FILE_WRITE, Buffer.from([0xE9, 0x00, 0x00, 0x00]));
  env.handleUpdateUpload(UPLOAD_FILE_ABORTED);
  env.handleUpdatePostComplete();

  assertEqual(env.bootPartition, "ota_0", 'Fault 4.3b: Boot partition remains ota_0 after abort');
  assertEqual(env.restartScheduled, false, 'Fault 4.3c: Reboot never scheduled on aborted update');
});

console.log('\n--- Suite 5: State Collision & Control Endpoint Locking ---');

test('Locking 5.1: ArduinoOTA rejected during active WebOTA without killing WebOTA', (env) => {
  env.handleUpdateUpload(UPLOAD_FILE_START);
  assertEqual(env.webOtaSuspended, true, 'Locking 5.1a: WebOTA active');

  const canService = env.handleOtaServiceCheck();
  assertEqual(canService, false, 'Locking 5.1b: handleOTA service check blocks ArduinoOTA');

  const ardRes = env.arduinoOtaStart();
  assertEqual(ardRes.status, 409, 'Locking 5.1c: ArduinoOTA start returns 409 Conflict');
  assertEqual(env.arduinoOtaActive, false, 'Locking 5.1d: ArduinoOTA flagged inactive');

  const chunk = Buffer.from([0xE9, 0x01, 0x02, 0x03]);
  const writeRes = env.handleUpdateUpload(UPLOAD_FILE_WRITE, chunk);
  assertEqual(writeRes.status, 200, 'Locking 5.1e: WebOTA continues writing normally');
});

test('Locking 5.2: WebOTA rejected during active ArduinoOTA without killing ArduinoOTA', (env) => {
  const ardRes = env.arduinoOtaStart();
  assertEqual(ardRes.status, 200, 'Locking 5.2a: ArduinoOTA started');
  assertEqual(env.arduinoOtaActive, true, 'Locking 5.2b: arduinoOtaActive set');

  const webRes = env.handleUpdateUpload(UPLOAD_FILE_START);
  assertEqual(webRes.status, 409, 'Locking 5.2c: WebOTA rejected with 409 Conflict');
  assertEqual(webRes.error, 'Firmware update already in progress', 'Locking 5.2d: Correct error text');

  // ArduinoOTA writes data
  env.Update.write(Buffer.from([0xE9, 0x01, 0x02, 0x03]), 4);

  const endRes = env.arduinoOtaEnd();
  assertEqual(endRes.status, 200, 'Locking 5.2e: ArduinoOTA finishes successfully');
  assertEqual(env.bootPartition, "ota_1", 'Locking 5.2f: Boot partition advanced to ota_1');
});

test('Locking 5.3: All control endpoints return HTTP 409 during firmware update', (env) => {
  env.suspendSafetyTask();

  const toggleRes = env.handleToggle();
  assertEqual(toggleRes.status, 409, 'Locking 5.3a: POST /toggle returns 409');

  const onRes = env.handleOn();
  assertEqual(onRes.status, 409, 'Locking 5.3b: POST /on returns 409');

  const offRes = env.handleOff();
  assertEqual(offRes.status, 409, 'Locking 5.3c: POST /off returns 409');

  const rebootRes = env.handleReboot();
  assertEqual(rebootRes.status, 409, 'Locking 5.3d: POST /reboot returns 409');

  const setupRes = env.handleSetupSave("NewSSID", "NewPass");
  assertEqual(setupRes.status, 409, 'Locking 5.3e: POST /setup returns 409');

  const calRes = env.handleCalibrateReset();
  assertEqual(calRes.status, 409, 'Locking 5.3f: POST /calibrate/reset returns 409');

  assertEqual(env.relayPulses, 0, 'Locking 5.3g: Zero relay pulses sent during update');
  assertEqual(env.restartScheduled, false, 'Locking 5.3h: Restart not scheduled');
});

console.log('\n--- Suite 6: Network Flap & Rollback Safeguards ---');

test('Rollback 6.1: Sub-30-second network flapping does not cancel rollback', (env) => {
  env.simulatedMillis = 10000;
  env.postWiFiEvent('ARDUINO_EVENT_WIFI_STA_GOT_IP');
  env.processEvents();

  env.simulatedMillis = 20000;
  env.postWiFiEvent('ARDUINO_EVENT_WIFI_STA_DISCONNECTED');
  env.processEvents();
  env.validateAppRollback();
  assertEqual(env.rollbackCancelled, false, 'Rollback 6.1a: Rollback NOT cancelled after 10s connection drop');

  env.simulatedMillis = 40000;
  env.postWiFiEvent('ARDUINO_EVENT_WIFI_STA_GOT_IP');
  env.processEvents();

  env.simulatedMillis = 50000;
  env.postWiFiEvent('ARDUINO_EVENT_WIFI_STA_DISCONNECTED');
  env.processEvents();
  env.validateAppRollback();
  assertEqual(env.rollbackCancelled, false, 'Rollback 6.1b: Rollback NOT cancelled after second 10s drop');

  env.simulatedMillis = 60000;
  env.postWiFiEvent('ARDUINO_EVENT_WIFI_STA_GOT_IP');
  env.processEvents();

  env.simulatedMillis = 89000;
  env.validateAppRollback();
  assertEqual(env.rollbackCancelled, false, 'Rollback 6.1c: Rollback NOT cancelled at 29s continuous');

  env.simulatedMillis = 91000;
  env.validateAppRollback();
  assertEqual(env.rollbackCancelled, true, 'Rollback 6.1d: Rollback cancelled after 31s continuous stable window');
  assertEqual(env.runningPartition.state, ESP_OTA_IMG_VALID, 'Rollback 6.1e: Partition marked VALID');
});

test('Rollback 6.2: Offline after 180s triggers automatic firmware rollback', (env) => {
  env.wifiConnected = false;
  env.simulatedMillis = 60000;
  env.validateAppRollback();
  assertEqual(env.rollbackCalled, false, 'Rollback 6.2a: Rollback not called at 60s offline');

  env.simulatedMillis = 180001;
  env.validateAppRollback();
  assertEqual(env.rollbackCalled, true, 'Rollback 6.2b: Automatic rollback called after 180s unreachable');
  assertEqual(env.runningPartition.state, ESP_OTA_IMG_INVALID, 'Rollback 6.2c: Partition marked INVALID');
  assertEqual(env.bootPartition, 'ota_prev', 'Rollback 6.2d: Boot partition rolled back to previous image');
});

test('Rollback 6.3: Standalone SoftAP validates after 30s uptime without rollback', (env) => {
  env.activeSsidPrimary = "";
  env.activeSsidBackup = "";
  env.wifiConnected = false;

  env.simulatedMillis = 15000;
  env.validateAppRollback();
  assertEqual(env.rollbackCancelled, false, 'Rollback 6.3a: Standalone setup not validated before 30s');

  env.simulatedMillis = 30001;
  env.validateAppRollback();
  assertEqual(env.rollbackCancelled, true, 'Rollback 6.3b: Standalone SoftAP validated at 30s');
  assertEqual(env.rollbackCalled, false, 'Rollback 6.3c: Standalone SoftAP never triggers rollback');
});

test('Reconnection 6.4: Reconnection loop coordination and SoftAP protection', (env) => {
  env.wifiConnected = false;
  env.apModeActive = true;
  env.softApStations = 1;
  env.simulatedMillis = 60000;

  assertEqual(env.handleWiFiReconnection(), 'IDLE', 'Reconnection 6.4a: Station reconnect deferred when user is on SoftAP');

  env.softApStations = 0;
  assertEqual(env.handleWiFiReconnection(), 'RECONNECT_TRIGGERED', 'Reconnection 6.4b: Station reconnect triggers when SoftAP is empty');

  env.apDisablePending = true;
  env.wifiConnected = true;
  assertEqual(env.handleWiFiReconnection(), 'CONNECTED', 'Reconnection 6.4c: Station connected');
  assertEqual(env.apModeActive, false, 'Reconnection 6.4d: SoftAP disabled safely');
});

test('Setup 6.5: Empty SSID rejected before suspension, valid SSID restarts', (env) => {
  const badRes = env.handleSetupSave("", "pass123");
  assertEqual(badRes.status, 400, 'Setup 6.5a: Empty SSID returns HTTP 400');
  assertEqual(env.safetyTaskPaused, false, 'Setup 6.5b: Safety task NOT suspended on bad input');

  const goodRes = env.handleSetupSave("OfficeWiFi", "TopSecret");
  assertEqual(goodRes.status, 200, 'Setup 6.5c: Valid SSID returns HTTP 200');
  assertEqual(env.safetyTaskPaused, true, 'Setup 6.5d: Safety task suspended before reboot');
  assertEqual(env.restartScheduled, true, 'Setup 6.5e: Restart scheduled');
});

console.log('\n--- Suite 7: HTTP Protocol & Header Injection Robustness ---');

test('Protocol 7.1: Zero-byte upload (Start directly followed by End) rejected cleanly', (env) => {
  env.handleUpdateUpload(UPLOAD_FILE_START);
  const endRes = env.handleUpdateUpload(UPLOAD_FILE_END);
  assertEqual(endRes.status, 500, 'Protocol 7.1a: Zero-byte upload rejected at UPLOAD_FILE_END');

  const postRes = env.handleUpdatePostComplete();
  assertEqual(postRes.status, 500, 'Protocol 7.1b: Post completion returns HTTP 500');
  assertEqual(env.bootPartition, "ota_0", 'Protocol 7.1c: Boot partition remains ota_0');
  assertEqual(env.safetyTaskPaused, false, 'Protocol 7.1d: Safety task unpaused');
});

test('Protocol 7.2: Malformed MD5 headers handled safely without crash', (env) => {
  const testMd5Header = (headerVal) => {
    env.resumeSafetyTask();
    env.webOtaSuspended = false;
    env.webOtaError = null;
    env.Update.expectedMD5 = null;
    env.handleUpdateUpload(UPLOAD_FILE_START, null, headerVal ? { 'x-MD5': headerVal } : {});
    return env.Update.expectedMD5;
  };

  // Non-hex string
  assertEqual(testMd5Header('zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz'), null, 'Protocol 7.2a: Non-hex MD5 ignored safely');

  // Truncated string (<32 chars)
  assertEqual(testMd5Header('12345'), null, 'Protocol 7.2b: Short MD5 ignored safely');

  // Excessive length (>32 chars)
  assertEqual(testMd5Header('0123456789abcdef0123456789abcdef999'), null, 'Protocol 7.2c: Long MD5 ignored safely');

  // Uppercase hex accepted and normalized to lowercase
  assertEqual(testMd5Header('A1B2C3D4E5F6A7B8C9D0E1F2A3B4C5D6'), 'a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6', 'Protocol 7.2d: Uppercase hex normalized to lowercase');
});

test('Protocol 7.3: Cross-origin and host spoofing attacks blocked with HTTP 403', (env) => {
  // External attacker origin
  const attackRes = env.handleUpdateUpload(UPLOAD_FILE_START, null, {
    'Origin': 'http://attacker-site.com',
    'Host': '192.168.1.33'
  });
  assertEqual(attackRes.status, 403, 'Protocol 7.3a: External origin rejected with 403');
  assertEqual(env.safetyTaskPaused, false, 'Protocol 7.3b: Safety task remains active on rejected origin');

  // Subdomain / Port spoofing
  const portSpoof = env.handleUpdateUpload(UPLOAD_FILE_START, null, {
    'Origin': 'http://192.168.1.33:8080',
    'Host': '192.168.1.33'
  });
  assertEqual(portSpoof.status, 403, 'Protocol 7.3c: Mismatched port origin rejected with 403');

  // Valid same-origin passes
  const validSameOrigin = env.handleUpdateUpload(UPLOAD_FILE_START, null, {
    'Origin': 'http://192.168.1.33',
    'Host': '192.168.1.33'
  });
  assertEqual(validSameOrigin.status, 200, 'Protocol 7.3d: Valid same-origin passes');
});

console.log('\n--- Suite 8: Seeded PRNG Stream Fuzzing & Bit-Flip Injection ---');

test('Fuzzer 8.1: 50 multi-pass fuzzing iterations with random chunk boundaries', (env) => {
  const prng = new Mulberry32(0xCAFEBABE);
  const payloadBase = Buffer.concat([
    Buffer.from([0xE9, 0x08, 0x02, 0x10]),
    prng.randomBytes(4092)
  ]);
  const validMD5 = crypto.createHash('md5').update(payloadBase).digest('hex');

  // Run 50 distinct random streaming splits
  for (let pass = 0; pass < 50; pass++) {
    const passEnv = new ESP32SystemMock();
    passEnv.handleUpdateUpload(UPLOAD_FILE_START, null, { 'x-MD5': validMD5 });

    let offset = 0;
    while (offset < payloadBase.length) {
      const remaining = payloadBase.length - offset;
      const chunkSize = (prng.next() < 0.1) ? 0 : Math.min(prng.nextInt(1, 1024), remaining);
      const chunk = (chunkSize === 0) ? Buffer.from([]) : payloadBase.slice(offset, offset + chunkSize);
      offset += chunkSize;
      passEnv.handleUpdateUpload(UPLOAD_FILE_WRITE, chunk);
    }

    const endRes = passEnv.handleUpdateUpload(UPLOAD_FILE_END);
    assertEqual(endRes.status, 200, `Fuzzer 8.1: Pass ${pass + 1}/50 streaming passed`);
    assertEqual(passEnv.Update.writtenBytes, payloadBase.length, `Fuzzer 8.1: Exact length verified on pass ${pass + 1}`);
    verifySystemInvariants(passEnv, `Fuzzer 8.1 pass ${pass + 1}`);
  }
});

test('Fuzzer 8.2: 25 bit-flip corrupted streams 100% blocked by verification', (env) => {
  const prng = new Mulberry32(0xDEADBEEF);
  const validBase = Buffer.concat([
    Buffer.from([0xE9, 0x04, 0x02, 0x10]),
    prng.randomBytes(2044)
  ]);
  const originalMD5 = crypto.createHash('md5').update(validBase).digest('hex');

  let blockedCount = 0;

  for (let iter = 0; iter < 25; iter++) {
    const corruptEnv = new ESP32SystemMock();
    corruptEnv.handleUpdateUpload(UPLOAD_FILE_START, null, { 'x-MD5': originalMD5 });

    // Copy payload and flip a single bit
    const corruptPayload = Buffer.from(validBase);
    const flipByte = prng.nextInt(0, corruptPayload.length - 1);
    const flipBit = 1 << prng.nextInt(0, 7);
    corruptPayload[flipByte] ^= flipBit;

    // Send payload
    const writeRes = corruptEnv.handleUpdateUpload(UPLOAD_FILE_WRITE, corruptPayload);
    const endRes = corruptEnv.handleUpdateUpload(UPLOAD_FILE_END);

    if (writeRes.status !== 200 || endRes.status !== 200) {
      blockedCount++;
    }
    verifySystemInvariants(corruptEnv, `Fuzzer 8.2 iter ${iter + 1}`);
  }

  assertEqual(blockedCount, 25, 'Fuzzer 8.2: 25/25 corrupted payloads blocked without escape');
});

console.log('\n--- Suite 9: 32-Bit millis() Rollover Invariance ---');

test('Rollover 9.1: Wi-Fi reconnection interval operates seamlessly across 0xFFFFFFFF', (env) => {
  env.wifiConnected = false;
  // Last attempt happened 5000ms before rollover
  env.lastWifiAttempt = (0xFFFFFFFF - 5000) >>> 0;

  // Current clock has wrapped around past 0 by 10001ms (total elapsed = 15001ms)
  env.simulatedMillis = 10001 >>> 0;

  assertEqual(env.handleWiFiReconnection(), 'RECONNECT_TRIGGERED', 'Rollover 9.1a: Reconnection triggered across 32-bit rollover');
  assertEqual(env.lastWifiAttempt, 10001 >>> 0, 'Rollover 9.1b: lastWifiAttempt updated past rollover');

  // Immediate next check 200ms later: elapsed = 200ms < 15000ms -> IDLE
  env.simulatedMillis = 10201 >>> 0;
  assertEqual(env.handleWiFiReconnection(), 'IDLE', 'Rollover 9.1c: Reconnect locked out during post-rollover interval');
});

test('Rollover 9.2: Rollback 30s stability window operates seamlessly across 0xFFFFFFFF', (env) => {
  // Wi-Fi connected 10,000ms before rollover
  env.wifiConnected = true;
  env.wifiConnectedSince = (0xFFFFFFFF - 10000) >>> 0;

  // Case 1: At 15,000ms after rollover (total connected = 25,001ms < 30,000ms)
  env.simulatedMillis = 15001 >>> 0;
  env.validateAppRollback();
  assertEqual(env.rollbackCancelled, false, 'Rollover 9.2a: Rollback NOT cancelled at 25s continuous across rollover');

  // Case 2: At 20,001ms after rollover (total connected = 30,002ms >= 30,000ms)
  env.simulatedMillis = 20002 >>> 0;
  env.validateAppRollback();
  assertEqual(env.rollbackCancelled, true, 'Rollover 9.2b: Rollback cancelled at 30s continuous across rollover');
  assertEqual(env.runningPartition.state, ESP_OTA_IMG_VALID, 'Rollover 9.2c: Partition validated past rollover');
});

console.log('\n--- Suite 10: 4KB SPI Flash Sector Alignment & Partial Erase ---');

test('Flash 10.1: Multi-sector binary writes span and erase exact 4KB sector count', (env) => {
  env.handleUpdateUpload(UPLOAD_FILE_START);

  // Payload of 12,288 bytes (exactly 3 sectors of 4096 bytes)
  const header = Buffer.from([0xE9, 0x01, 0x02, 0x03]);
  const body = Buffer.concat([header, crypto.randomBytes(12288 - 4)]);

  env.handleUpdateUpload(UPLOAD_FILE_WRITE, body);
  env.handleUpdateUpload(UPLOAD_FILE_END);

  assertEqual(env.Update.erasedSectors.size, 3, 'Flash 10.1a: Exactly 3 flash sectors spanned and erased');
  assertEqual(env.Update.erasedSectors.has(0), true, 'Flash 10.1b: Sector 0 tracked');
  assertEqual(env.Update.erasedSectors.has(1), true, 'Flash 10.1c: Sector 1 tracked');
  assertEqual(env.Update.erasedSectors.has(2), true, 'Flash 10.1d: Sector 2 tracked');
});

test('Flash 10.2: Aborted upload cleanly purges partially written sectors', (env) => {
  env.handleUpdateUpload(UPLOAD_FILE_START);

  // Write 6000 bytes (spans sector 0 and enters sector 1)
  const partial = Buffer.concat([Buffer.from([0xE9, 0x00]), crypto.randomBytes(5998)]);
  env.handleUpdateUpload(UPLOAD_FILE_WRITE, partial);

  assertEqual(env.Update.sectorWrites.size, 2, 'Flash 10.2a: Partial write spans 2 sectors in RAM');

  // Client aborts transfer
  env.handleUpdateUpload(UPLOAD_FILE_ABORTED);

  assertEqual(env.Update.sectorWrites.size, 0, 'Flash 10.2b: Sector write buffers purged on abort');
  assertEqual(env.bootPartition, "ota_0", 'Flash 10.2c: Running boot partition preserved');
});

console.log('\n--- Suite 11: Asynchronous Event Storms & Task Preemption ---');

test('Storm 11.1: 50 rapid Wi-Fi event bursts during active WebOTA do not deadlock', (env) => {
  env.handleUpdateUpload(UPLOAD_FILE_START);
  assertEqual(env.isFirmwareUpdating(), true, 'Storm 11.1a: Safety task suspended for WebOTA');

  // Flood event queue with 50 rapid alternating Wi-Fi events while stream writes
  const payload = Buffer.concat([Buffer.from([0xE9, 0x02]), crypto.randomBytes(2046)]);

  for (let burst = 0; burst < 50; burst++) {
    const event = (burst % 2 === 0) ? 'ARDUINO_EVENT_WIFI_STA_GOT_IP' : 'ARDUINO_EVENT_WIFI_STA_DISCONNECTED';
    env.postWiFiEvent(event);
  }

  // Interleave chunk writes with event processing
  env.handleUpdateUpload(UPLOAD_FILE_WRITE, payload.slice(0, 1024));
  env.processEvents(); // Process event queue during flash write
  env.handleUpdateUpload(UPLOAD_FILE_WRITE, payload.slice(1024));

  // Reconnection loop execution during active update
  const reconnectStatus = env.handleWiFiReconnection();
  assertEqual(reconnectStatus, 'BLOCKED_BY_OTA', 'Storm 11.1b: Reconnection loop cleanly blocked during event burst');

  const endRes = env.handleUpdateUpload(UPLOAD_FILE_END);
  assertEqual(endRes.status, 200, 'Storm 11.1c: WebOTA completed without deadlock despite event storm');
  const postRes = env.handleUpdatePostComplete();
  assertEqual(postRes.status, 200, 'Storm 11.1d: Post completion returns HTTP 200');
  assertEqual(env.bootPartition, "ota_1", 'Storm 11.1e: Boot partition successfully committed');
});

test('Storm 11.2: Sensor contact during update attempt preserves update rejection', (env) => {
  env.currentState = STATE_STOPPED;

  // Simultaneously initiate door motion while update request hits webserver
  env.currentState = STATE_OPENING;

  const res = env.handleUpdateUpload(UPLOAD_FILE_START);
  assertEqual(res.status, 409, 'Storm 11.2a: Motion preemption immediately rejected');
  assertEqual(env.safetyTaskPaused, false, 'Storm 11.2b: Safety task remains active on Core 0');
  assertEqual(env.relayActive, false, 'Storm 11.2c: Relay remains de-energized');
});

console.log('\n====================================================');
console.log(`  OTA TEST RESULTS: ${passedTests}/${totalTests} Passed (${failedTests} Failed)`);
console.log('====================================================\n');

if (failedTests > 0) {
  process.exit(1);
} else {
  process.exit(0);
}
