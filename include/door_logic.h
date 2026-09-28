#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "door_types.h"

extern volatile DoorState currentState;
extern volatile DoorState previousState;
extern unsigned long lastStateChangeTime;
extern unsigned long lastDebounceTimeClosed;
extern unsigned long lastDebounceTimeOpen;
extern bool realClosedSensor;
extern bool realOpenSensor;
extern bool lastReadingClosed;
extern bool lastReadingOpen;

// Relay pulse state
extern volatile bool relayActive;
extern volatile unsigned long relayTriggerTime;
extern volatile DoorState pendingState;
extern int pendingPulsesCount;

void releaseRelayIfExpired();

// Diagnostic flags
extern bool obstacleWarning;
extern bool sensorFault;
extern bool sensorTimeoutError;
extern bool failedToMove;
extern bool midTrackStall;
extern DoorState lastCommandedDirection;
extern unsigned long lastPulseTime;
extern bool pulseVerificationPending;
extern DoorState pulseOriginState;

extern SemaphoreHandle_t doorStateMutex;

struct DoorStateLock {
  bool acquired;
  DoorStateLock(TickType_t timeout = pdMS_TO_TICKS(500));
  ~DoorStateLock();
};

// Calibration & Position Tracking
extern unsigned long openDurationMs;
extern unsigned long closeDurationMs;
extern unsigned long nvsStoredOpenMs;
extern unsigned long nvsStoredCloseMs;
extern bool isCalibrated;
extern uint8_t currentPositionPct;
extern uint8_t startPositionPct;
extern unsigned long activeFlightStartTime;
extern bool switchUnseated;
extern unsigned long lastCalibrationTimeOpen;
extern unsigned long lastCalibrationTimeClose;
extern unsigned long lastNVSWriteTime;

void initDoorHardware();
void saveStateToNVS();
void checkSensorsWithDebounce();
void updateLogic();
void triggerRelay();
void handleRelay();
void clearFaultFlags();
uint8_t calculateCurrentPosition();
void handleCalibrateReset();

void handleToggle();
void handleOn();
void handleOff();

