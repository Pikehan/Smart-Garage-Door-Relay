#include "door_logic.h"
#include "web_portal.h"

DoorState currentState = STATE_UNKNOWN;
DoorState previousState = STATE_UNKNOWN;

unsigned long lastStateChangeTime = 0;
unsigned long lastDebounceTimeClosed = 0;
unsigned long lastDebounceTimeOpen = 0;
bool realClosedSensor = false;
bool realOpenSensor = false;
bool lastReadingClosed = false;
bool lastReadingOpen = false;

// Relay pulse state
bool relayActive = false;
unsigned long relayTriggerTime = 0;
DoorState pendingState = STATE_UNKNOWN;
int pendingPulsesCount = 0;

Preferences preferences;

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

portMUX_TYPE doorLogicMux = portMUX_INITIALIZER_UNLOCKED;

void clearFaultFlags() {
  obstacleWarning = false;
  sensorFault = false;
  sensorTimeoutError = false;
  failedToMove = false;
  midTrackStall = false;
}

void initDoorHardware() {
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

  preferences.begin("garage", true);
  uint8_t savedCurr = preferences.getUChar("curr", (uint8_t)STATE_STOPPED);
  uint8_t savedPrev = preferences.getUChar("prev", (uint8_t)STATE_UNKNOWN);
  preferences.end();

  // If a limit switch is contacted, set state directly
  if (initialClose && !initialOpen) {
    currentState = STATE_CLOSED;
    previousState = STATE_CLOSED;
    lastCommandedDirection = STATE_OPENING;
    Serial.println("BOOT (1st Order): CLOSED limit switch active. State set to CLOSED.");
    saveStateToNVS();
  }
  else if (initialOpen && !initialClose) {
    currentState = STATE_OPEN;
    previousState = STATE_OPEN;
    lastCommandedDirection = STATE_CLOSING;
    Serial.println("BOOT (1st Order): OPEN limit switch active. State set to OPEN.");
    saveStateToNVS();
  }
  else {
    // Neither sensor active (mid-travel): hold stopped and recover direction from NVS
    currentState = STATE_STOPPED;

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

  preferences.begin("garage", false);
  uint8_t existingCurr = preferences.getUChar("curr", 255);
  uint8_t existingPrev = preferences.getUChar("prev", 255);

  if (existingCurr != (uint8_t)currentState || existingPrev != (uint8_t)previousState) {
    preferences.putUChar("curr", (uint8_t)currentState);
    preferences.putUChar("prev", (uint8_t)previousState);
    preferences.putBool("s_close", realClosedSensor);
    preferences.putBool("s_open", realOpenSensor);
  }
  preferences.end();
}

void triggerRelay() {
  Serial.println("ACTION: Toggling Relay Pulse (200ms)");
  digitalWrite(RELAY_PIN, LOW);
  relayActive = true;
  relayTriggerTime = millis();
  lastPulseTime = relayTriggerTime;
}

void handleRelay() {
  if (relayActive && (millis() - relayTriggerTime >= RELAY_PRESS_TIME)) {
    digitalWrite(RELAY_PIN, HIGH);
    relayActive = false;
  }

  // Handle queued pulses for reverse/stop actions
  if (pendingState != STATE_UNKNOWN && !relayActive && (millis() - relayTriggerTime >= PENDING_RELAY_DELAY)) {
    triggerRelay();

    if (pendingPulsesCount > 1) {
      pendingPulsesCount--;
    } else {
      previousState = currentState;
      currentState = pendingState;
      lastStateChangeTime = millis();
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
  sendCORSHeaders();

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
    server.send(200, "application/json", "{\"status\":\"success\", \"message\":\"Cancelled pending movement. Door remains stopped.\"}");
    return;
  }

  // Rate limit rapid triggers
  if (lastPulseTime > 0 && (millis() - lastPulseTime < COMMAND_LOCKOUT_MS)) {
    server.send(429, "application/json", "{\"status\":\"error\", \"message\":\"Command rate limit: Please wait 1.5s between triggers.\"}");
    return;
  }

  clearFaultFlags();

  if (relayActive) {
    server.send(429, "application/json", "{\"status\":\"error\", \"message\":\"Relay is actively pressing. Please wait.\"}");
    return;
  }

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
  }
  else if (currentState == STATE_STOPPED || currentState == STATE_UNKNOWN) {
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
  }
  else if (currentState == STATE_OPEN) {
    nextState = STATE_CLOSING;
    lastCommandedDirection = STATE_CLOSING;
  }

  if (nextState != currentState) {
    previousState = currentState;
    currentState = nextState;
    lastStateChangeTime = millis();
    Serial.printf("Toggle State Transition: %s\n", getDebugString(currentState));
    saveStateToNVS();
  }

  server.send(200, "application/json", "{\"status\":\"success\", \"message\":\"Relay triggered\"}");
}

void handleOn() {
  sendCORSHeaders();

  if (lastPulseTime > 0 && (millis() - lastPulseTime < COMMAND_LOCKOUT_MS)) {
    server.send(429, "application/json", "{\"status\":\"error\", \"message\":\"Command rate limit: Please wait 1.5s between triggers.\"}");
    return;
  }

  // Require manual toggle to clear stall faults
  if (midTrackStall || failedToMove) {
    server.send(409, "application/json", "{\"status\":\"error\", \"message\":\"Movement locked due to stall or failed-to-move fault. Use POST /toggle to manually override.\"}");
    return;
  }

  if (pendingState == STATE_OPENING || currentState == STATE_OPEN || currentState == STATE_OPENING) {
    server.send(200, "application/json", "{\"status\":\"ignored\", \"message\":\"Door is already open or opening.\"}");
    return;
  }

  if (relayActive || pendingState != STATE_UNKNOWN) {
    server.send(429, "application/json", "{\"status\":\"error\", \"message\":\"Relay is actively pressing or pending sequence. Please wait.\"}");
    return;
  }

  if (currentState == STATE_CLOSED) {
    pulseVerificationPending = true;
    pulseOriginState = STATE_CLOSED;
  }

  triggerRelay();
  lastCommandedDirection = STATE_OPENING;

  if (currentState == STATE_CLOSING) {
    previousState = currentState;
    currentState = STATE_STOPPED;
    pendingState = STATE_OPENING;
    pendingPulsesCount = 1;
    saveStateToNVS();
    server.send(200, "application/json", "{\"status\":\"success\", \"message\":\"Relay triggered to STOP closing; queued OPEN in 500ms.\"}");
  }
  else if (currentState == STATE_STOPPED && previousState == STATE_OPENING) {
    pendingState = STATE_OPENING;
    pendingPulsesCount = 2;
    saveStateToNVS();
    server.send(200, "application/json", "{\"status\":\"success\", \"message\":\"Relay sequence started; queued OPEN in 500ms.\"}");
  }
  else {
    previousState = currentState;
    currentState = STATE_OPENING;
    lastStateChangeTime = millis();
    pendingState = STATE_UNKNOWN;
    pendingPulsesCount = 0;
    server.send(200, "application/json", "{\"status\":\"success\", \"message\":\"Relay triggered to OPEN door.\"}");
  }
}

void handleOff() {
  sendCORSHeaders();

  if (lastPulseTime > 0 && (millis() - lastPulseTime < COMMAND_LOCKOUT_MS)) {
    server.send(429, "application/json", "{\"status\":\"error\", \"message\":\"Command rate limit: Please wait 1.5s between triggers.\"}");
    return;
  }

  // Require manual toggle to clear stall faults
  if (midTrackStall || failedToMove) {
    server.send(409, "application/json", "{\"status\":\"error\", \"message\":\"Movement locked due to stall or failed-to-move fault. Use POST /toggle to manually override.\"}");
    return;
  }

  if (pendingState == STATE_CLOSING || currentState == STATE_CLOSED || currentState == STATE_CLOSING) {
    server.send(200, "application/json", "{\"status\":\"ignored\", \"message\":\"Door is already closed or closing.\"}");
    return;
  }

  if (relayActive || pendingState != STATE_UNKNOWN) {
    server.send(429, "application/json", "{\"status\":\"error\", \"message\":\"Relay is actively pressing or pending sequence. Please wait.\"}");
    return;
  }

  if (currentState == STATE_OPEN) {
    pulseVerificationPending = true;
    pulseOriginState = STATE_OPEN;
  }

  triggerRelay();
  lastCommandedDirection = STATE_CLOSING;

  if (currentState == STATE_OPENING) {
    previousState = currentState;
    currentState = STATE_STOPPED;
    pendingState = STATE_CLOSING;
    pendingPulsesCount = 1;
    saveStateToNVS();
    server.send(200, "application/json", "{\"status\":\"success\", \"message\":\"Relay triggered to STOP opening; queued CLOSE in 500ms.\"}");
  }
  else if (currentState == STATE_STOPPED && previousState == STATE_CLOSING) {
    pendingState = STATE_CLOSING;
    pendingPulsesCount = 2;
    saveStateToNVS();
    server.send(200, "application/json", "{\"status\":\"success\", \"message\":\"Relay sequence started; queued CLOSE in 500ms.\"}");
  }
  else {
    previousState = currentState;
    currentState = STATE_CLOSING;
    lastStateChangeTime = millis();
    pendingState = STATE_UNKNOWN;
    pendingPulsesCount = 0;
    server.send(200, "application/json", "{\"status\":\"success\", \"message\":\"Relay triggered to CLOSE door.\"}");
  }
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
    if (currentState != STATE_STOPPED) {
      Serial.println("FAULT: Both Open and Closed limit switches reading active! Forcing STOPPED.");
    }
    detectedState = STATE_STOPPED;
  }

  // Closed switch reached
  else if (realClosedSensor) {
    if (currentState == STATE_OPENING && previousState == STATE_CLOSED && (millis() - lastStateChangeTime < 1500)) {
      // Ignore closed contact during initial 1.5s of opening
      detectedState = STATE_OPENING;
    } else {
      if (currentState == STATE_OPENING) {
        Serial.println("DIRECTION CORRECTION / OBSTACLE: Expected OPENING, but CLOSED limit switch triggered! Flagging OBSTACLE_WARNING.");
        obstacleWarning = true;
      }
      detectedState = STATE_CLOSED;
      pendingState = STATE_UNKNOWN;
      pendingPulsesCount = 0;
      failedToMove = false;
    }
  }
  // Open switch reached
  else if (realOpenSensor) {
    if (currentState == STATE_CLOSING && previousState == STATE_OPEN && (millis() - lastStateChangeTime < 1500)) {
      // Ignore open contact during initial 1.5s of closing
      detectedState = STATE_CLOSING;
    } else {
      if (currentState == STATE_CLOSING) {
        Serial.println("DIRECTION CORRECTION / OBSTACLE: Expected CLOSING, but OPEN limit switch triggered! Flagging OBSTACLE_WARNING.");
        obstacleWarning = true;
      }
      detectedState = STATE_OPEN;
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
      pendingState = STATE_UNKNOWN;
      pendingPulsesCount = 0;
      failedToMove = false;
    }
    else if (currentState == STATE_OPEN) {
      Serial.println("MANUAL OVERRIDE: OPEN switch opened externally! Transitioning to CLOSING.");
      detectedState = STATE_CLOSING;
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
    Serial.println("TIMEOUT / STALL: Door movement exceeded 22 seconds without hitting limit switch. Forcing STOPPED.");
  }

  if (detectedState != currentState) {
    previousState = currentState;
    currentState = detectedState;
    lastStateChangeTime = millis();
    Serial.printf("State Transition: %s\n", getDebugString(currentState));
    saveStateToNVS();
  }
}

