#pragma once
#include <Arduino.h>
#include <IPAddress.h>

// Include optional local credentials header if present and not overridden by build flags
#if __has_include("secrets.h")
  #ifndef WIFI_SSID_PRIMARY
    #include "secrets.h"
  #endif
#endif

// Wi-Fi defaults (empty strings fallback to NVS Flash credentials)
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

// Hardware pins
const int RELAY_PIN = 2;
const int OPEN_SENSOR_PIN = 25;
const int CLOSED_SENSOR_PIN = 26;

// Timing parameters (ms)
const unsigned long MOVEMENT_TIMEOUT = 27000;    // Max travel time before timeout (~17-25s typical + margin)
const unsigned long DEBOUNCE_DELAY = 50;         // Switch debounce window
const unsigned long RELAY_PRESS_TIME = 200;      // Motor button pulse duration
const unsigned long PENDING_RELAY_DELAY = 500;   // Delay between stop and reverse pulses
const unsigned long COMMAND_LOCKOUT_MS = 1500;   // Rate limit between user triggers
const unsigned long SENSOR_DISENGAGE_TIMEOUT = 2500; // Timeout waiting for limit switch release
const unsigned long RECONNECT_INTERVAL_MS = 10000; // Reconnect check interval

// Calibration & Position Tracking parameters
const unsigned long DEFAULT_OPEN_DURATION_MS = 17000;   // Default open travel duration
const unsigned long DEFAULT_CLOSE_DURATION_MS = 17000;  // Default close travel duration
const unsigned long MIN_TRAVEL_TIME_MS = 15000;          // Minimum plausible travel duration (15s)
const unsigned long MAX_TRAVEL_TIME_MS = 25000;          // Maximum plausible travel duration (25s)
const unsigned long CALIBRATION_NVS_MIN_DELTA_MS = 200;  // Minimum cumulative drift to write NVS (200ms)
const unsigned long CALIBRATION_MAX_DEVIATION_MS = 3500; // Maximum allowed deviation from baseline (3.5s)
const unsigned long CALIBRATION_INTERVAL_MS = 86400000UL;// 24 hours between calibration runs
const unsigned long NVS_WRITE_COOLDOWN_MS = 300000UL;    // 5 minutes cooldown between NVS flash writes

// Feature flags
#ifndef ENABLE_SERIAL_DEBUG
  #define ENABLE_SERIAL_DEBUG 1
#endif
#ifndef ENABLE_ARDUINO_OTA
  #define ENABLE_ARDUINO_OTA 1           // Set to 0 to save ~45KB flash if only web update is used
#endif
#ifndef ENABLE_WIFI_SLEEP
  #define ENABLE_WIFI_SLEEP 0            // Set to 0 to prevent 100-1000ms latency spikes; 1 for modem sleep
#endif
#ifndef ENABLE_WEB_AUTH
  #define ENABLE_WEB_AUTH 1              // Enable basic auth on sensitive endpoints
#endif
#ifndef WEB_AUTH_PROTECT_ACTUATORS
  #define WEB_AUTH_PROTECT_ACTUATORS 0   // 0 = open local actuators; 1 = require auth for /toggle, /on, /off
#endif
#ifndef WEB_AUTH_USER
  #define WEB_AUTH_USER "admin"
#endif
#ifndef WEB_AUTH_PASS
  #define WEB_AUTH_PASS "garage1234"
#endif
#ifndef DEFAULT_AP_PASS
  #define DEFAULT_AP_PASS "garage1234"
#endif

// Static IP settings
const IPAddress LOCAL_IP(192, 168, 1, 33);
const IPAddress GATEWAY(192, 168, 1, 1);
const IPAddress SUBNET(255, 255, 255, 0);
const IPAddress PRIMARY_DNS(8, 8, 8, 8);
const IPAddress SECONDARY_DNS(8, 8, 4, 4);

const char DEVICE_HOSTNAME[] = "garage-door";

