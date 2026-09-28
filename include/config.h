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
const unsigned long MOVEMENT_TIMEOUT = 22000;    // Max travel time before timeout (~18s typical)
const unsigned long DEBOUNCE_DELAY = 50;         // Switch debounce window
const unsigned long RELAY_PRESS_TIME = 200;      // Motor button pulse duration
const unsigned long PENDING_RELAY_DELAY = 500;   // Delay between stop and reverse pulses
const unsigned long COMMAND_LOCKOUT_MS = 1500;   // Rate limit between user triggers
const unsigned long SENSOR_DISENGAGE_TIMEOUT = 2500; // Timeout waiting for limit switch release
const unsigned long RECONNECT_INTERVAL_MS = 10000; // Reconnect check interval

// Feature flags
#ifndef ENABLE_SERIAL_DEBUG
  #define ENABLE_SERIAL_DEBUG 1
#endif
#ifndef ENABLE_ARDUINO_OTA
  #define ENABLE_ARDUINO_OTA 1           // Set to 0 to save ~45KB flash if only web update is used
#endif
#ifndef ENABLE_WIFI_SLEEP
  #define ENABLE_WIFI_SLEEP 1            // Modem sleep to reduce idle power/heat
#endif

// Static IP settings
const IPAddress LOCAL_IP(192, 168, 1, 33);
const IPAddress GATEWAY(192, 168, 1, 1);
const IPAddress SUBNET(255, 255, 255, 0);
const IPAddress PRIMARY_DNS(8, 8, 8, 8);
const IPAddress SECONDARY_DNS(8, 8, 4, 4);

const char DEVICE_HOSTNAME[] = "garage-door";

