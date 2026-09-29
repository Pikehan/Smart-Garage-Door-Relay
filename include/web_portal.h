#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include "config.h"

extern WebServer server;

void initWiFi();
void handleWiFiReconnection();
void initWebPortal();
void handleWebClients();
void handleOTA();
void validateAppRollback();

// Web endpoints and helpers
bool isSameOriginRequest();
void sendReadOnlyCORSHeaders();
void sendCORSHeaders();
void handleOptions();
void handleGetState();
void handleRoot();
void handleNotFound();
void handleUpdateForm();
void handleUpdateUpload();
void handleSetupForm();
void handleSetupSave();
void handleReboot();
void handleCalibrateReset();
#if ENABLE_ARDUINO_OTA
void setupOTA();
#endif

// NVS Wi-Fi credentials
bool loadWiFiCredentials(String& ssidPri, String& passPri, String& ssidBak, String& passBak);
void saveWiFiCredentials(const char* ssidPri, const char* passPri, const char* ssidBak = "", const char* passBak = "");
extern bool apModeActive;


