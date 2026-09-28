#include <Arduino.h>
#include "config.h"
#include "door_logic.h"
#include "web_portal.h"

// Core 0 task for sensor polling and relay timing
void doorSafetyTask(void *pvParameters) {
#if ENABLE_SERIAL_DEBUG
  unsigned long lastStackCheck = 0;
#endif
  for (;;) {
    // Unconditionally de-assert active relay pulse regardless of mutex lock state
    releaseRelayIfExpired();

    if (doorStateMutex != NULL && xSemaphoreTakeRecursive(doorStateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      handleRelay();
      checkSensorsWithDebounce();
      updateLogic();
      xSemaphoreGiveRecursive(doorStateMutex);
    }

#if ENABLE_SERIAL_DEBUG
    if (millis() - lastStackCheck > 60000) {
      lastStackCheck = millis();
      UBaseType_t highWaterMark = uxTaskGetStackHighWaterMark(NULL);
      Serial.printf("[Task] doorSafetyTask Stack High-Water: %u bytes remaining\n", (unsigned int)(highWaterMark * sizeof(StackType_t)));
    }
#endif

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32 GARAGE DOOR CONTROLLER BOOT ===");
  initDoorHardware();
  xTaskCreatePinnedToCore(doorSafetyTask, "SafetyTask", 4096, NULL, 5, NULL, 0);
  initWiFi();
  WiFi.setSleep(ENABLE_WIFI_SLEEP ? true : false);
  initWebPortal();
  validateAppRollback();
}

void loop() {
  handleWiFiReconnection();
  handleOTA();
  handleWebClients();
  vTaskDelay(pdMS_TO_TICKS(5));
}