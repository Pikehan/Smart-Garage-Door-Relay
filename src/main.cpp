#include <Arduino.h>
#include "config.h"
#include "door_logic.h"
#include "web_portal.h"

static TaskHandle_t safetyTaskHandle = NULL;
static volatile bool safetyTaskPaused = false;

void suspendSafetyTask() {
  if (doorStateMutex != NULL) {
    xSemaphoreTakeRecursive(doorStateMutex, portMAX_DELAY);
  }
  safetyTaskPaused = true;
  digitalWrite(RELAY_PIN, HIGH);
  relayActive = false;
  pendingState = STATE_UNKNOWN;
  pendingPulsesCount = 0;
  if (doorStateMutex != NULL) {
    xSemaphoreGiveRecursive(doorStateMutex);
  }
  Serial.println("[Task] doorSafetyTask paused for OTA flash operation.");
}

void resumeSafetyTask() {
  if (doorStateMutex != NULL) {
    xSemaphoreTakeRecursive(doorStateMutex, portMAX_DELAY);
  }
  safetyTaskPaused = false;
  if (doorStateMutex != NULL) {
    xSemaphoreGiveRecursive(doorStateMutex);
  }
  Serial.println("[Task] doorSafetyTask resumed.");
}

bool isFirmwareUpdating() {
  return safetyTaskPaused;
}

// Core 0 task for sensor polling and relay timing
void doorSafetyTask(void *pvParameters) {
#if ENABLE_SERIAL_DEBUG
  unsigned long lastStackCheck = 0;
#endif
  for (;;) {
    if (safetyTaskPaused) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (doorStateMutex != NULL && xSemaphoreTakeRecursive(doorStateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (!safetyTaskPaused) {
        handleRelay();
        checkSensorsWithDebounce();
        updateLogic();
      }
      xSemaphoreGiveRecursive(doorStateMutex);
    }

#if ENABLE_SERIAL_DEBUG
    if (!safetyTaskPaused && (millis() - lastStackCheck > 60000)) {
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
  xTaskCreatePinnedToCore(doorSafetyTask, "SafetyTask", 4096, NULL, 5, &safetyTaskHandle, 0);
  initWiFi();
  WiFi.setSleep(ENABLE_WIFI_SLEEP ? true : false);
  initWebPortal();
}

void loop() {
  handleWiFiReconnection();
  validateAppRollback();
  handleOTA();
  handleWebClients();
  vTaskDelay(pdMS_TO_TICKS(5));
}