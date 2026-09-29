#include "door_logic.h"
#include "web_portal.h"

volatile DoorState currentState = STATE_UNKNOWN;
volatile DoorState previousState = STATE_UNKNOWN;

unsigned long lastStateChangeTime = 0;
unsigned long lastDebounceTimeClosed = 0;
unsigned long lastDebounceTimeOpen = 0;
bool realClosedSensor = false;
bool realOpenSensor = false;
bool lastReadingClosed = false;
bool lastReadingOpen = false;

// Relay pulse state
volatile bool relayActive = false;
volatile unsigned long relayTriggerTime = 0;
volatile DoorState pendingState = STATE_UNKNOWN;
int pendingPulsesCount = 0;

// Diagnostics
bool obstacleWarning = false;
bool sensorFault = false;
bool sensorTimeoutError = false;
bool failedToMove = false;
bool midTrackStall = false;
DoorState lastCommandedDirection = STATE_UNKNOWN;
unsigned long lastPulseTime = 0;
bool pulseVerificationPending = false;
DoorState pulseOriginState = STATE_UNKNOWN;

// Calibration & Position Tracking
unsigned long openDurationMs = DEFAULT_OPEN_DURATION_MS;
unsigned long closeDurationMs = DEFAULT_CLOSE_DURATION_MS;
unsigned long nvsStoredOpenMs = DEFAULT_OPEN_DURATION_MS;
unsigned long nvsStoredCloseMs = DEFAULT_CLOSE_DURATION_MS;
bool isCalibrated = false;
uint8_t currentPositionPct = 0;
uint8_t startPositionPct = 0;
unsigned long activeFlightStartTime = 0;
bool switchUnseated = false;
unsigned long lastCalibrationTimeOpen = 0;
unsigned long lastCalibrationTimeClose = 0;
unsigned long lastNVSWriteTime = 0;
bool hasIntermediateStop = false;

SemaphoreHandle_t doorStateMutex = NULL;

DoorStateLock::DoorStateLock(TickType_t timeout) {
  acquired = (doorStateMutex != NULL && xSemaphoreTakeRecursive(doorStateMutex, timeout) == pdTRUE);
}

DoorStateLock::~DoorStateLock() {
  if (acquired && doorStateMutex != NULL) {
    xSemaphoreGiveRecursive(doorStateMutex);
  }
}

void clearFaultFlags() {
  obstacleWarning = false;
  sensorFault = false;
  sensorTimeoutError = false;
  failedToMove = false;
  midTrackStall = false;
}

uint8_t calculateCurrentPosition() {
  if (currentState == STATE_CLOSED) {
    return 0;
  }
  if (currentState == STATE_OPEN) {
    return 100;
  }
  if (currentState == STATE_STOPPED) {
    return currentPositionPct;
  }
  if (currentState == STATE_OPENING) {
    if (activeFlightStartTime == 0) return startPositionPct;
    unsigned long elapsed = millis() - activeFlightStartTime;
    unsigned long dur = (openDurationMs >= MIN_TRAVEL_TIME_MS) ? openDurationMs : DEFAULT_OPEN_DURATION_MS;
    unsigned long addedPct = (elapsed * 100UL) / dur;
    unsigned long pos = (unsigned long)startPositionPct + addedPct;
    if (pos > 100) pos = 100;
    return (uint8_t)pos;
  }
  if (currentState == STATE_CLOSING) {
    if (activeFlightStartTime == 0) return startPositionPct;
    unsigned long elapsed = millis() - activeFlightStartTime;
    unsigned long dur = (closeDurationMs >= MIN_TRAVEL_TIME_MS) ? closeDurationMs : DEFAULT_CLOSE_DURATION_MS;
    unsigned long subPct = (elapsed * 100UL) / dur;
    if (subPct >= (unsigned long)startPositionPct) {
      return 0;
    }
    return (uint8_t)(startPositionPct - subPct);
  }
  return currentPositionPct;
}

void initDoorHardware() {
  if (doorStateMutex == NULL) {
    doorStateMutex = xSemaphoreCreateRecursiveMutex();
  }

  // Active-low relay: set HIGH before OUTPUT mode to prevent spurious pulse on boot
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(OPEN_SENSOR_PIN, INPUT_PULLUP);
  pinMode(CLOSED_SENSOR_PIN, INPUT_PULLUP);

  pendingState = STATE_UNKNOWN;
  pendingPulsesCount = 0;
  pulseVerificationPending = false;

  bool initialClose = !digitalRead(CLOSED_SENSOR_PIN);
  bool initialOpen = !digitalRead(OPEN_SENSOR_PIN);

  realClosedSensor = initialClose;
  realOpenSensor = initialOpen;
  lastReadingClosed = initialClose;
  lastReadingOpen = initialOpen;

  Preferences prefs;
  prefs.begin("garage", true);
  uint8_t savedCurr = prefs.getUChar("curr", (uint8_t)STATE_STOPPED);
  uint8_t savedPrev = prefs.getUChar("prev", (uint8_t)STATE_UNKNOWN);
  openDurationMs = prefs.getUInt("open_ms", DEFAULT_OPEN_DURATION_MS);
  closeDurationMs = prefs.getUInt("close_ms", DEFAULT_CLOSE_DURATION_MS);
  isCalibrated = prefs.getBool("cal_done", false);
  uint8_t savedStopPos = prefs.getUChar("stop_pos", 0);
  prefs.end();

  // Divide-by-zero & math sanity clamping
  if (openDurationMs < MIN_TRAVEL_TIME_MS || openDurationMs > MAX_TRAVEL_TIME_MS) {
    openDurationMs = DEFAULT_OPEN_DURATION_MS;
  }
  if (closeDurationMs < MIN_TRAVEL_TIME_MS || closeDurationMs > MAX_TRAVEL_TIME_MS) {
    closeDurationMs = DEFAULT_CLOSE_DURATION_MS;
  }
  nvsStoredOpenMs = openDurationMs;
  nvsStoredCloseMs = closeDurationMs;

  if (isCalibrated) {
    lastCalibrationTimeOpen = millis();
    lastCalibrationTimeClose = millis();
  } else {
    lastCalibrationTimeOpen = 0;
    lastCalibrationTimeClose = 0;
  }

  // If a limit switch is contacted, set state directly
  if (initialClose && !initialOpen) {
    currentState = STATE_CLOSED;
    previousState = STATE_CLOSED;
    lastCommandedDirection = STATE_OPENING;
    currentPositionPct = 0;
    startPositionPct = 0;
    switchUnseated = false;
    Serial.println("BOOT (1st Order): CLOSED limit switch active. State set to CLOSED.");
    saveStateToNVS();
  }
  else if (initialOpen && !initialClose) {
    currentState = STATE_OPEN;
    previousState = STATE_OPEN;
    lastCommandedDirection = STATE_CLOSING;
    currentPositionPct = 100;
    startPositionPct = 100;
    switchUnseated = false;
    Serial.println("BOOT (1st Order): OPEN limit switch active. State set to OPEN.");
    saveStateToNVS();
  }
  else {
    // Neither sensor active (mid-travel): hold stopped and recover direction from NVS
    currentState = STATE_STOPPED;
    currentPositionPct = (savedStopPos <= 100) ? savedStopPos : 50;
    startPositionPct = currentPositionPct;
    switchUnseated = false;

    if (savedCurr == STATE_OPENING || savedPrev == STATE_OPENING) {
      previousState = STATE_OPENING;
      lastCommandedDirection = STATE_CLOSING;
      Serial.println("BOOT (2nd Order): Pre-power loss movement recovered -> WAS OPENING.");
    }
    else if (savedCurr == STATE_CLOSING || savedPrev == STATE_CLOSING) {
      previousState = STATE_CLOSING;
      lastCommandedDirection = STATE_OPENING;
      Serial.println("BOOT (2nd Order): Pre-power loss movement recovered -> WAS CLOSING.");
    }
    else if (savedPrev != STATE_UNKNOWN) {
      previousState = (DoorState)savedPrev;
      lastCommandedDirection = (previousState == STATE_OPENING) ? STATE_CLOSING : STATE_OPENING;
      Serial.printf("BOOT (2nd Order): Recovered previousState -> %s.\n", getDebugString(previousState));
    }
    else {
      previousState = STATE_UNKNOWN;
      lastCommandedDirection = STATE_UNKNOWN;
      Serial.println("BOOT (2nd Order): No prior state in NVS. Defaulting previousState to UNKNOWN.");
    }

    saveStateToNVS();
  }
}

// Only commit resting states to NVS to avoid flash wear during movement
void saveStateToNVS() {
  if (currentState != STATE_CLOSED && currentState != STATE_OPEN && currentState != STATE_STOPPED) {
    return;
  }

  Preferences prefs;
  prefs.begin("garage", false);
  uint8_t existingCurr = prefs.getUChar("curr", 255);
  uint8_t existingPrev = prefs.getUChar("prev", 255);

  if (existingCurr != (uint8_t)currentState || existingPrev != (uint8_t)previousState) {
    prefs.putUChar("curr", (uint8_t)currentState);
    prefs.putUChar("prev", (uint8_t)previousState);
    prefs.putBool("s_close", realClosedSensor);
    prefs.putBool("s_open", realOpenSensor);
  }

  if (currentState == STATE_CLOSED) {
    prefs.putUChar("stop_pos", 0);
  } else if (currentState == STATE_OPEN) {
    prefs.putUChar("stop_pos", 100);
  } else if (currentState == STATE_STOPPED) {
    prefs.putUChar("stop_pos", currentPositionPct);
  }

  // Flash write storm protection:
  // Only write when resting at CLOSED, cooldown >= 5 min, and cumulative drift >= 200ms
  unsigned long now = millis();
  if (currentState == STATE_CLOSED && (now - lastNVSWriteTime >= NVS_WRITE_COOLDOWN_MS || lastNVSWriteTime == 0)) {
    bool nvsUpdated = false;
    long openDelta = (long)openDurationMs - (long)nvsStoredOpenMs;
    if (labs(openDelta) >= (long)CALIBRATION_NVS_MIN_DELTA_MS) {
      prefs.putUInt("open_ms", (uint32_t)openDurationMs);
      nvsStoredOpenMs = openDurationMs;
      nvsUpdated = true;
    }
    long closeDelta = (long)closeDurationMs - (long)nvsStoredCloseMs;
    if (labs(closeDelta) >= (long)CALIBRATION_NVS_MIN_DELTA_MS) {
      prefs.putUInt("close_ms", (uint32_t)closeDurationMs);
      nvsStoredCloseMs = closeDurationMs;
      nvsUpdated = true;
    }
    if (nvsUpdated) {
      prefs.putBool("cal_done", isCalibrated);
      lastNVSWriteTime = now;
      Serial.printf("NVS Flash Commit: open_ms=%lu, close_ms=%lu\n", openDurationMs, closeDurationMs);
    }
  }

  prefs.end();
}

void handleCalibrateReset() {
  if (!isSameOriginRequest()) {
    server.send(403, "application/json", "{\"status\":\"error\", \"message\":\"Cross-origin request forbidden\"}");
    return;
  }
  if (isFirmwareUpdating()) {
    server.send(409, "application/json", "{\"status\":\"error\", \"message\":\"Firmware update in progress. Controls locked.\"}");
    return;
  }
  {
    DoorStateLock lock;
    if (!lock.acquired) {
      server.send(503, "application/json", "{\"status\":\"error\", \"message\":\"Door controller busy\"}");
      return;
    }

    openDurationMs = DEFAULT_OPEN_DURATION_MS;
    closeDurationMs = DEFAULT_CLOSE_DURATION_MS;
    nvsStoredOpenMs = DEFAULT_OPEN_DURATION_MS;
    nvsStoredCloseMs = DEFAULT_CLOSE_DURATION_MS;
    isCalibrated = false;
    lastCalibrationTimeOpen = 0;
    lastCalibrationTimeClose = 0;

    Preferences prefs;
    prefs.begin("garage", false);
    prefs.putUInt("open_ms", (uint32_t)DEFAULT_OPEN_DURATION_MS);
    prefs.putUInt("close_ms", (uint32_t)DEFAULT_CLOSE_DURATION_MS);
    prefs.putBool("cal_done", false);
    prefs.end();
  }

  Serial.println("ACTION: Calibration reset to defaults (17.0s) & daily quota cleared.");
  server.send(200, "application/json", "{\"status\":\"success\", \"message\":\"Calibration and daily quota reset! Next flight will re-calibrate immediately.\"}");
}

void triggerRelay() {
  Serial.println("ACTION: Toggling Relay Pulse (200ms)");
  relayTriggerTime = millis();
  lastPulseTime = relayTriggerTime;
  digitalWrite(RELAY_PIN, LOW);
  relayActive = true;
}

void releaseRelayIfExpired() {
  if (relayActive && (millis() - relayTriggerTime >= RELAY_PRESS_TIME)) {
    digitalWrite(RELAY_PIN, HIGH);
    relayActive = false;
  }
}

void handleRelay() {
  releaseRelayIfExpired();

  // Handle queued pulses for reverse/stop actions
  if (pendingState != STATE_UNKNOWN && !relayActive && (millis() - relayTriggerTime >= PENDING_RELAY_DELAY)) {
    triggerRelay();

    if (pendingPulsesCount > 1) {
      pendingPulsesCount--;
    } else {
      previousState = currentState;
      currentState = pendingState;
      lastStateChangeTime = millis();
      activeFlightStartTime = relayTriggerTime;
      startPositionPct = currentPositionPct;
      hasIntermediateStop = true;
      switchUnseated = true;
      Serial.printf("Auto-Reverse State Transition: %s\n", getDebugString(currentState));
      saveStateToNVS();
      pendingState = STATE_UNKNOWN;
      pendingPulsesCount = 0;
    }
  }

  // Verify limit switch opens after pulsing
  if (pulseVerificationPending && (millis() - lastPulseTime >= SENSOR_DISENGAGE_TIMEOUT)) {
    if ((pulseOriginState == STATE_CLOSED && realClosedSensor) ||
        (pulseOriginState == STATE_OPEN && realOpenSensor)) {
      failedToMove = true;
      Serial.println("FAULT: Pulse sent but limit switch failed to disengage! Motor locked or unpowered.");
      if (currentState == STATE_OPENING || currentState == STATE_CLOSING) {
        currentState = STATE_STOPPED;
        saveStateToNVS();
      }
    }
    pulseVerificationPending = false;
  }
}

void handleToggle() {
  if (!isSameOriginRequest()) {
    server.send(403, "application/json", "{\"status\":\"error\", \"message\":\"Cross-origin request forbidden\"}");
    return;
  }
  if (isFirmwareUpdating()) {
    server.send(409, "application/json", "{\"status\":\"error\", \"message\":\"Firmware update in progress. Controls locked.\"}");
    return;
  }

  int httpCode = 200;
  const char* responseJson = "{\"status\":\"success\", \"message\":\"Relay triggered\"}";

  {
    DoorStateLock lock;
    if (!lock.acquired) {
      server.send(503, "application/json", "{\"status\":\"error\", \"message\":\"Door controller busy\"}");
      return;
    }

    // Cancel pending multi-pulse sequence
    if (pendingState != STATE_UNKNOWN) {
      bool motorMoving = (pendingPulsesCount == 2);
      pendingState = STATE_UNKNOWN;
      pendingPulsesCount = 0;
      currentState = STATE_STOPPED;
      lastStateChangeTime = millis();
      saveStateToNVS();
      if (motorMoving) {
        triggerRelay();
      }
      clearFaultFlags();
      httpCode = 200;
      responseJson = "{\"status\":\"success\", \"message\":\"Cancelled pending movement. Door remains stopped.\"}";
    }
    // Rate limit rapid triggers
    else if (lastPulseTime > 0 && (millis() - lastPulseTime < COMMAND_LOCKOUT_MS)) {
      httpCode = 429;
      responseJson = "{\"status\":\"error\", \"message\":\"Command rate limit: Please wait 1.5s between triggers.\"}";
    }
    else if (relayActive) {
      httpCode = 429;
      responseJson = "{\"status\":\"error\", \"message\":\"Relay is actively pressing. Please wait.\"}";
    }
    else {
      clearFaultFlags();

      // Verify sensor disengages after pulsing
      if (currentState == STATE_CLOSED || currentState == STATE_OPEN) {
        pulseVerificationPending = true;
        pulseOriginState = currentState;
      } else {
        pulseVerificationPending = false;
      }

      // Pulse relay (wall button toggle behavior)
      triggerRelay();

      DoorState nextState = currentState;

      if (currentState == STATE_OPENING || currentState == STATE_CLOSING) {
        nextState = STATE_STOPPED;
        currentPositionPct = calculateCurrentPosition();
        startPositionPct = currentPositionPct;
        hasIntermediateStop = true;
        switchUnseated = false;
      }
      else if (currentState == STATE_STOPPED || currentState == STATE_UNKNOWN) {
        startPositionPct = currentPositionPct;
        hasIntermediateStop = true;
        switchUnseated = true;
        activeFlightStartTime = millis();

        if (realClosedSensor && realOpenSensor) {
          // Sensor fault: alternate direction
          lastCommandedDirection = (lastCommandedDirection == STATE_OPENING) ? STATE_CLOSING : STATE_OPENING;
          nextState = lastCommandedDirection;
        }
        else if (previousState == STATE_OPENING) nextState = STATE_CLOSING;
        else if (previousState == STATE_CLOSING) nextState = STATE_OPENING;
        else if (previousState == STATE_CLOSED) nextState = STATE_OPENING;
        else if (previousState == STATE_OPEN) nextState = STATE_CLOSING;
        else {
          lastCommandedDirection = (lastCommandedDirection == STATE_OPENING) ? STATE_CLOSING : STATE_OPENING;
          nextState = lastCommandedDirection;
        }
      }
      else if (currentState == STATE_CLOSED) {
        nextState = STATE_OPENING;
        lastCommandedDirection = STATE_OPENING;
        startPositionPct = 0;
        switchUnseated = true;
        activeFlightStartTime = relayTriggerTime;
        hasIntermediateStop = false;
      }
      else if (currentState == STATE_OPEN) {
        nextState = STATE_CLOSING;
        lastCommandedDirection = STATE_CLOSING;
        startPositionPct = 100;
        switchUnseated = true;
        activeFlightStartTime = relayTriggerTime;
        hasIntermediateStop = false;
      }

      if (nextState != currentState) {
        previousState = currentState;
        currentState = nextState;
        lastStateChangeTime = millis();
        Serial.printf("Toggle State Transition: %s\n", getDebugString(currentState));
        saveStateToNVS();
      }

      httpCode = 200;
      responseJson = "{\"status\":\"success\", \"message\":\"Relay triggered\"}";
    }
  }

  server.send(httpCode, "application/json", responseJson);
}

void handleOn() {
  if (!isSameOriginRequest()) {
    server.send(403, "application/json", "{\"status\":\"error\", \"message\":\"Cross-origin request forbidden\"}");
    return;
  }
  if (isFirmwareUpdating()) {
    server.send(409, "application/json", "{\"status\":\"error\", \"message\":\"Firmware update in progress. Controls locked.\"}");
    return;
  }

  int httpCode = 200;
  const char* responseJson = "";

  {
    DoorStateLock lock;
    if (!lock.acquired) {
      server.send(503, "application/json", "{\"status\":\"error\", \"message\":\"Door controller busy\"}");
      return;
    }

    if (lastPulseTime > 0 && (millis() - lastPulseTime < COMMAND_LOCKOUT_MS)) {
      httpCode = 429;
      responseJson = "{\"status\":\"error\", \"message\":\"Command rate limit: Please wait 1.5s between triggers.\"}";
    }
    // Require manual toggle to clear stall faults
    else if (midTrackStall || failedToMove) {
      httpCode = 409;
      responseJson = "{\"status\":\"error\", \"message\":\"Movement locked due to stall or failed-to-move fault. Use POST /toggle to manually override.\"}";
    }
    else if (pendingState == STATE_OPENING || currentState == STATE_OPEN || currentState == STATE_OPENING) {
      httpCode = 200;
      responseJson = "{\"status\":\"ignored\", \"message\":\"Door is already open or opening.\"}";
    }
    else if (relayActive || pendingState != STATE_UNKNOWN) {
      httpCode = 429;
      responseJson = "{\"status\":\"error\", \"message\":\"Relay is actively pressing or pending sequence. Please wait.\"}";
    }
    else {
      clearFaultFlags();

      triggerRelay();
      lastCommandedDirection = STATE_OPENING;

      if (currentState == STATE_CLOSED) {
        pulseVerificationPending = true;
        pulseOriginState = STATE_CLOSED;
        startPositionPct = 0;
        switchUnseated = true;
        activeFlightStartTime = relayTriggerTime;
        hasIntermediateStop = false;
      } else if (currentState == STATE_STOPPED) {
        startPositionPct = currentPositionPct;
        hasIntermediateStop = true;
        switchUnseated = true;
        activeFlightStartTime = relayTriggerTime;
      }

      if (currentState == STATE_CLOSING) {
        currentPositionPct = calculateCurrentPosition();
        startPositionPct = currentPositionPct;
        hasIntermediateStop = true;
        switchUnseated = false;
        previousState = currentState;
        currentState = STATE_STOPPED;
        pendingState = STATE_OPENING;
        pendingPulsesCount = 1;
        saveStateToNVS();
        httpCode = 200;
        responseJson = "{\"status\":\"success\", \"message\":\"Relay triggered to STOP closing; queued OPEN in 500ms.\"}";
      }
      else if (currentState == STATE_STOPPED && previousState == STATE_OPENING) {
        pendingState = STATE_OPENING;
        pendingPulsesCount = 2;
        saveStateToNVS();
        httpCode = 200;
        responseJson = "{\"status\":\"success\", \"message\":\"Relay sequence started; queued OPEN in 500ms.\"}";
      }
      else {
        previousState = currentState;
        currentState = STATE_OPENING;
        lastStateChangeTime = millis();
        pendingState = STATE_UNKNOWN;
        pendingPulsesCount = 0;
        httpCode = 200;
        responseJson = "{\"status\":\"success\", \"message\":\"Relay triggered to OPEN door.\"}";
      }
    }
  }

  server.send(httpCode, "application/json", responseJson);
}

void handleOff() {
  if (!isSameOriginRequest()) {
    server.send(403, "application/json", "{\"status\":\"error\", \"message\":\"Cross-origin request forbidden\"}");
    return;
  }
  if (isFirmwareUpdating()) {
    server.send(409, "application/json", "{\"status\":\"error\", \"message\":\"Firmware update in progress. Controls locked.\"}");
    return;
  }

  int httpCode = 200;
  const char* responseJson = "";

  {
    DoorStateLock lock;
    if (!lock.acquired) {
      server.send(503, "application/json", "{\"status\":\"error\", \"message\":\"Door controller busy\"}");
      return;
    }

    if (lastPulseTime > 0 && (millis() - lastPulseTime < COMMAND_LOCKOUT_MS)) {
      httpCode = 429;
      responseJson = "{\"status\":\"error\", \"message\":\"Command rate limit: Please wait 1.5s between triggers.\"}";
    }
    // Require manual toggle to clear stall faults
    else if (midTrackStall || failedToMove) {
      httpCode = 409;
      responseJson = "{\"status\":\"error\", \"message\":\"Movement locked due to stall or failed-to-move fault. Use POST /toggle to manually override.\"}";
    }
    else if (pendingState == STATE_CLOSING || currentState == STATE_CLOSED || currentState == STATE_CLOSING) {
      httpCode = 200;
      responseJson = "{\"status\":\"ignored\", \"message\":\"Door is already closed or closing.\"}";
    }
    else if (relayActive || pendingState != STATE_UNKNOWN) {
      httpCode = 429;
      responseJson = "{\"status\":\"error\", \"message\":\"Relay is actively pressing or pending sequence. Please wait.\"}";
    }
    else {
      clearFaultFlags();

      triggerRelay();
      lastCommandedDirection = STATE_CLOSING;

      if (currentState == STATE_OPEN) {
        pulseVerificationPending = true;
        pulseOriginState = STATE_OPEN;
        startPositionPct = 100;
        switchUnseated = true;
        activeFlightStartTime = relayTriggerTime;
        hasIntermediateStop = false;
      } else if (currentState == STATE_STOPPED) {
        startPositionPct = currentPositionPct;
        hasIntermediateStop = true;
        switchUnseated = true;
        activeFlightStartTime = relayTriggerTime;
      }

      if (currentState == STATE_OPENING) {
        currentPositionPct = calculateCurrentPosition();
        startPositionPct = currentPositionPct;
        hasIntermediateStop = true;
        switchUnseated = false;
        previousState = currentState;
        currentState = STATE_STOPPED;
        pendingState = STATE_CLOSING;
        pendingPulsesCount = 1;
        saveStateToNVS();
        httpCode = 200;
        responseJson = "{\"status\":\"success\", \"message\":\"Relay triggered to STOP opening; queued CLOSE in 500ms.\"}";
      }
      else if (currentState == STATE_STOPPED && previousState == STATE_CLOSING) {
        pendingState = STATE_CLOSING;
        pendingPulsesCount = 2;
        saveStateToNVS();
        httpCode = 200;
        responseJson = "{\"status\":\"success\", \"message\":\"Relay sequence started; queued CLOSE in 500ms.\"}";
      }
      else {
        previousState = currentState;
        currentState = STATE_CLOSING;
        lastStateChangeTime = millis();
        pendingState = STATE_UNKNOWN;
        pendingPulsesCount = 0;
        httpCode = 200;
        responseJson = "{\"status\":\"success\", \"message\":\"Relay triggered to CLOSE door.\"}";
      }
    }
  }

  server.send(httpCode, "application/json", responseJson);
}

// Debounce limit switch inputs
void checkSensorsWithDebounce() {
  bool readingClosed = !digitalRead(CLOSED_SENSOR_PIN);
  bool readingOpen = !digitalRead(OPEN_SENSOR_PIN);

  if (readingClosed != lastReadingClosed) {
    lastDebounceTimeClosed = millis();
  }
  if (readingOpen != lastReadingOpen) {
    lastDebounceTimeOpen = millis();
  }

  if ((millis() - lastDebounceTimeClosed) > DEBOUNCE_DELAY) {
    realClosedSensor = readingClosed;
  }
  if ((millis() - lastDebounceTimeOpen) > DEBOUNCE_DELAY) {
    realOpenSensor = readingOpen;
  }

  lastReadingClosed = readingClosed;
  lastReadingOpen = readingOpen;
}

// State machine evaluation
void updateLogic() {
  DoorState detectedState = currentState;

  if (pulseVerificationPending) {
    if ((pulseOriginState == STATE_CLOSED && !realClosedSensor) ||
        (pulseOriginState == STATE_OPEN && !realOpenSensor)) {
      pulseVerificationPending = false;
      failedToMove = false;
    }
  }

  // Both limit switches active indicates sensor or wiring fault
  if (realClosedSensor && realOpenSensor) {
    sensorFault = true;
    hasIntermediateStop = true;
    if (currentState != STATE_STOPPED) {
      Serial.println("FAULT: Both Open and Closed limit switches reading active! Forcing STOPPED.");
    }
    detectedState = STATE_STOPPED;
  }

  // Closed switch reached
  else if (realClosedSensor) {
    if (currentState == STATE_OPENING && previousState == STATE_CLOSED && (millis() - lastStateChangeTime < SENSOR_DISENGAGE_TIMEOUT)) {
      // Ignore closed contact during initial disengage timeout
      detectedState = STATE_OPENING;
    } else {
      if (currentState == STATE_OPENING) {
        Serial.println("DIRECTION CORRECTION / OBSTACLE: Expected OPENING, but CLOSED limit switch triggered! Flagging OBSTACLE_WARNING.");
        obstacleWarning = true;
        hasIntermediateStop = true;
      } else if (currentState == STATE_CLOSING) {
        // Successful clean close arrival
        if (activeFlightStartTime > 0 && !hasIntermediateStop) {
          unsigned long rawFlightMs = millis() - activeFlightStartTime;
          unsigned long measuredFlightMs = rawFlightMs + 1000UL; // +1s switch activation area compensation
          bool inWindow = (measuredFlightMs >= MIN_TRAVEL_TIME_MS && measuredFlightMs <= MAX_TRAVEL_TIME_MS);
          bool noOutlier = (!isCalibrated || (labs((long)measuredFlightMs - (long)closeDurationMs) <= (long)CALIBRATION_MAX_DEVIATION_MS));
          bool dailyReady = (!isCalibrated || (millis() - lastCalibrationTimeClose >= CALIBRATION_INTERVAL_MS || lastCalibrationTimeClose == 0));

          if (inWindow && noOutlier && dailyReady) {
            closeDurationMs = (closeDurationMs * 3UL + measuredFlightMs) / 4UL; // EMA smooth
            isCalibrated = true;
            lastCalibrationTimeClose = millis();
            Serial.printf("CALIBRATION: CLOSE duration calibrated: %lu ms (raw: %lu ms + 1000ms activation area)\n", closeDurationMs, rawFlightMs);
          }
        }
      }
      detectedState = STATE_CLOSED;
      currentPositionPct = 0;
      startPositionPct = 0;
      switchUnseated = false;
      pendingState = STATE_UNKNOWN;
      pendingPulsesCount = 0;
      failedToMove = false;
    }
  }
  // Open switch reached
  else if (realOpenSensor) {
    if (currentState == STATE_CLOSING && previousState == STATE_OPEN && (millis() - lastStateChangeTime < SENSOR_DISENGAGE_TIMEOUT)) {
      // Ignore open contact during initial disengage timeout
      detectedState = STATE_CLOSING;
    } else {
      if (currentState == STATE_CLOSING) {
        Serial.println("DIRECTION CORRECTION / OBSTACLE: Expected CLOSING, but OPEN limit switch triggered! Flagging OBSTACLE_WARNING.");
        obstacleWarning = true;
        hasIntermediateStop = true;
      } else if (currentState == STATE_OPENING) {
        // Successful clean open arrival
        if (activeFlightStartTime > 0 && !hasIntermediateStop) {
          unsigned long rawFlightMs = millis() - activeFlightStartTime;
          unsigned long measuredFlightMs = rawFlightMs + 1000UL; // +1s switch activation area compensation
          bool inWindow = (measuredFlightMs >= MIN_TRAVEL_TIME_MS && measuredFlightMs <= MAX_TRAVEL_TIME_MS);
          bool noOutlier = (!isCalibrated || (labs((long)measuredFlightMs - (long)openDurationMs) <= (long)CALIBRATION_MAX_DEVIATION_MS));
          bool dailyReady = (!isCalibrated || (millis() - lastCalibrationTimeOpen >= CALIBRATION_INTERVAL_MS || lastCalibrationTimeOpen == 0));

          if (inWindow && noOutlier && dailyReady) {
            openDurationMs = (openDurationMs * 3UL + measuredFlightMs) / 4UL; // EMA smooth
            isCalibrated = true;
            lastCalibrationTimeOpen = millis();
            Serial.printf("CALIBRATION: OPEN duration calibrated: %lu ms (raw: %lu ms + 1000ms activation area)\n", openDurationMs, rawFlightMs);
          }
        }
      }
      detectedState = STATE_OPEN;
      currentPositionPct = 100;
      startPositionPct = 100;
      switchUnseated = false;
      pendingState = STATE_UNKNOWN;
      pendingPulsesCount = 0;
      failedToMove = false;
    }
  }
  // Detect manual movement if a limit switch unseats without command
  else if (!realClosedSensor && !realOpenSensor) {
    if (currentState == STATE_CLOSED) {
      Serial.println("MANUAL OVERRIDE: CLOSED switch opened externally! Transitioning to OPENING.");
      detectedState = STATE_OPENING;
      startPositionPct = 0;
      switchUnseated = true;
      activeFlightStartTime = millis();
      hasIntermediateStop = true; // External manual strokes do not auto-calibrate
      pendingState = STATE_UNKNOWN;
      pendingPulsesCount = 0;
      failedToMove = false;
    }
    else if (currentState == STATE_OPEN) {
      Serial.println("MANUAL OVERRIDE: OPEN switch opened externally! Transitioning to CLOSING.");
      detectedState = STATE_CLOSING;
      startPositionPct = 100;
      switchUnseated = true;
      activeFlightStartTime = millis();
      hasIntermediateStop = true; // External manual strokes do not auto-calibrate
      pendingState = STATE_UNKNOWN;
      pendingPulsesCount = 0;
      failedToMove = false;
    }
  }

  // Watchdog timeout if travel exceeds limit
  if ((detectedState == STATE_OPENING || detectedState == STATE_CLOSING) &&
      (millis() - lastStateChangeTime >= MOVEMENT_TIMEOUT)) {
    detectedState = STATE_STOPPED;
    sensorTimeoutError = true;
    sensorFault = true;
    midTrackStall = true;
    hasIntermediateStop = true;
    currentPositionPct = calculateCurrentPosition();
    startPositionPct = currentPositionPct;
    switchUnseated = false;
    Serial.println("TIMEOUT / STALL: Door movement exceeded timeout without hitting limit switch. Forcing STOPPED.");
  }

  if (detectedState != currentState) {
    previousState = currentState;
    currentState = detectedState;
    lastStateChangeTime = millis();
    Serial.printf("State Transition: %s\n", getDebugString(currentState));
    saveStateToNVS();
  }
}

