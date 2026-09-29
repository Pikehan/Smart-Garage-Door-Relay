#pragma once
#include <Arduino.h>
#include <IPAddress.h>

#if __has_include("secrets.h")
#ifndef WIFI_SSID_PRIMARY
#include "secrets.h"
#endif
#endif

#ifndef WIFI_SSID_PRIMARY
#define WIFI_SSID_PRIMARY ""
#endif
#ifndef WIFI_PASS_PRIMARY
#define WIFI_PASS_PRIMARY ""
#endif
#ifndef WIFI_SSID_BACKUP
#define WIFI_SSID_BACKUP ""
#endif
#ifndef WIFI_PASS_BACKUP
#define WIFI_PASS_BACKUP ""
#endif

#ifndef RELAY_PIN
#define RELAY_PIN 27
#endif
const int OPEN_SENSOR_PIN = 25;
const int CLOSED_SENSOR_PIN = 26;

// Timing parameters (ms)
const unsigned long MOVEMENT_TIMEOUT = 27000;
const unsigned long DEBOUNCE_DELAY = 50;
const unsigned long RELAY_PRESS_TIME = 200;
const unsigned long PENDING_RELAY_DELAY = 500;
const unsigned long COMMAND_LOCKOUT_MS = 1500;
const unsigned long SENSOR_DISENGAGE_TIMEOUT = 2500;
const unsigned long RECONNECT_INTERVAL_MS = 10000;

// Calibration & Position Tracking (ms)
const unsigned long DEFAULT_OPEN_DURATION_MS = 17000;
const unsigned long DEFAULT_CLOSE_DURATION_MS = 17000;
const unsigned long MIN_TRAVEL_TIME_MS = 15000;
const unsigned long MAX_TRAVEL_TIME_MS = 25000;
const unsigned long CALIBRATION_NVS_MIN_DELTA_MS = 200;
const unsigned long CALIBRATION_MAX_DEVIATION_MS = 3500;
const unsigned long CALIBRATION_INTERVAL_MS = 86400000UL;
const unsigned long NVS_WRITE_COOLDOWN_MS = 300000UL;

const unsigned long REED_SWITCH_OPEN_OFFSET_MS = 3100;
const unsigned long REED_SWITCH_CLOSE_OFFSET_MS = 900;

#ifndef ENABLE_SERIAL_DEBUG
#define ENABLE_SERIAL_DEBUG 1
#endif
#ifndef ENABLE_ARDUINO_OTA
#define ENABLE_ARDUINO_OTA 1
#endif
#ifndef ENABLE_WIFI_SLEEP
#define ENABLE_WIFI_SLEEP 0
#endif
#ifndef ENABLE_STATIC_IP
#define ENABLE_STATIC_IP 1
#endif
#ifndef DEFAULT_AP_PASS
#define DEFAULT_AP_PASS "garage1234"
#endif

// Network settings
const IPAddress LOCAL_IP(192, 168, 1, 33);
const IPAddress GATEWAY(192, 168, 1, 1);
const IPAddress SUBNET(255, 255, 255, 0);
const IPAddress PRIMARY_DNS(8, 8, 8, 8);
const IPAddress SECONDARY_DNS(8, 8, 4, 4);

const char DEVICE_HOSTNAME[] = "garage-door";
