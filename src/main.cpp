#include <Arduino.h>
#include "config.h"
#include "door_logic.h"
#include "web_portal.h"

// Core 0 task for sensor polling and relay timing
void doorSafetyTask(void *pvParameters) {
  for (;;) {
    handleRelay();
    checkSensorsWithDebounce();
    updateLogic();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n======================================");
  Serial.println("   ESP32 GARAGE DOOR CONTROLLER BOOT   ");
  Serial.println("======================================");

  initDoorHardware();

  // Run limit switch and relay loop on Core 0
  xTaskCreatePinnedToCore(doorSafetyTask, "SafetyTask", 2560, NULL, 5, NULL, 0);

  initWiFi();
#if ENABLE_WIFI_SLEEP
  WiFi.setSleep(true);
#endif

  initWebPortal();
  validateAppRollback();
}

void loop() {
  handleWiFiReconnection();
  handleOTA();
  handleWebClients();
  vTaskDelay(pdMS_TO_TICKS(5));
}