#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "door_types.h"

extern DoorState currentState;
extern DoorState previousState;
extern unsigned long lastStateChangeTime;
extern unsigned long lastDebounceTimeClosed;
extern unsigned long lastDebounceTimeOpen;
extern bool realClosedSensor;
extern bool realOpenSensor;
extern bool lastReadingClosed;
extern bool lastReadingOpen;

// Relay pulse state
extern bool relayActive;
extern unsigned long relayTriggerTime;
extern DoorState pendingState;
extern int pendingPulsesCount;

extern Preferences preferences;

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

extern portMUX_TYPE doorLogicMux;

void initDoorHardware();
void saveStateToNVS();
void checkSensorsWithDebounce();
void updateLogic();
void triggerRelay();
void handleRelay();
void clearFaultFlags();

void handleToggle();
void handleOn();
void handleOff();

