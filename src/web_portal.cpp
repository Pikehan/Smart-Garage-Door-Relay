#include "web_portal.h"
#include "door_logic.h"
#include "config.h"
#include <WiFi.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <esp_ota_ops.h>
#include <Preferences.h>
#include <ESPmDNS.h>

WebServer server(80);
static int currentNetwork = 1;
static unsigned long lastWifiAttempt = 0;
static volatile bool wifiConnected = false;
static volatile unsigned long wifiConnectedSince = 0;
bool apModeActive = false;
static bool webOtaSuspended = false;
static bool arduinoOtaActive = false;

static String activeSsidPrimary;
static String activePassPrimary;
static String activeSsidBackup;
static String activePassBackup;

static Preferences wifiPrefs;

void sendReadOnlyCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void sendCORSHeaders() {
  sendReadOnlyCORSHeaders();
}

void handleOptions() {
  sendReadOnlyCORSHeaders();
  server.send(204, "text/plain", "");
}

bool isSameOriginRequest() {
  String target = server.hasHeader("Origin") ? server.header("Origin") : (server.hasHeader("Referer") ? server.header("Referer") : "");
  if (target.length() == 0) return true;

  int start = 0;
  if (target.startsWith("http://")) start = 7;
  else if (target.startsWith("https://")) start = 8;
  int end = target.indexOf('/', start);
  String host = (end != -1) ? target.substring(start, end) : target.substring(start);
  int portIdx = host.indexOf(':');
  if (portIdx != -1) {
    String port = host.substring(portIdx + 1);
    if (port != "80") {
      Serial.printf("[Security] Blocked non-standard port origin: %s\n", target.c_str());
      return false;
    }
    host = host.substring(0, portIdx);
  }

  // Strict identity checks to prevent DNS rebinding attacks
  if (host.equalsIgnoreCase(DEVICE_HOSTNAME) ||
      host.equalsIgnoreCase(String(DEVICE_HOSTNAME) + ".local") ||
      host == WiFi.localIP().toString() ||
      host == WiFi.softAPIP().toString() ||
      host == "localhost" || host == "127.0.0.1") {
    return true;
  }
  Serial.printf("[Security] Blocked cross-origin request from: %s (Parsed Host: %s)\n", target.c_str(), host.c_str());
  return false;
}

bool loadWiFiCredentials(String& ssidPri, String& passPri, String& ssidBak, String& passBak) {
  wifiPrefs.begin("wifi_cfg", true);
  ssidPri = wifiPrefs.getString("ssid_pri", "");
  passPri = wifiPrefs.getString("pass_pri", "");
  ssidBak = wifiPrefs.getString("ssid_bak", "");
  passBak = wifiPrefs.getString("pass_bak", "");
  wifiPrefs.end();
  return (ssidPri.length() > 0);
}

void saveWiFiCredentials(const char* ssidPri, const char* passPri, const char* ssidBak, const char* passBak) {
  wifiPrefs.begin("wifi_cfg", false);
  if (ssidPri && strlen(ssidPri) > 0) {
    wifiPrefs.putString("ssid_pri", ssidPri);
    activeSsidPrimary = String(ssidPri);
  }
  if (passPri) {
    wifiPrefs.putString("pass_pri", passPri);
    activePassPrimary = String(passPri);
  }
  if (ssidBak && strlen(ssidBak) > 0) {
    wifiPrefs.putString("ssid_bak", ssidBak);
    activeSsidBackup = String(ssidBak);
  } else {
    wifiPrefs.remove("ssid_bak");
    activeSsidBackup = "";
  }
  if (passBak && strlen(passBak) > 0) {
    wifiPrefs.putString("pass_bak", passBak);
    activePassBackup = String(passBak);
  } else {
    wifiPrefs.remove("pass_bak");
    activePassBackup = "";
  }
  wifiPrefs.end();
  Serial.println("[WiFi] Credentials committed to NVS Flash.");
}

static void startSoftAPPortal() {
  if (apModeActive) return;
  apModeActive = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("Garage-Door-Setup", DEFAULT_AP_PASS);
  Serial.print("[WiFi] SoftAP started! Connect to 'Garage-Door-Setup' at IP: ");
  Serial.println(WiFi.softAPIP());
}

static volatile bool mdnsPending = false;
static volatile bool apDisablePending = false;

void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiConnected = true;
      wifiConnectedSince = millis();
      Serial.print("\n[WiFi] Connected! IP Address: ");
      Serial.println(WiFi.localIP());
      mdnsPending = true;
      if (apModeActive) {
        apDisablePending = true;
      }
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiConnected = false;
      wifiConnectedSince = 0;
      Serial.println("\n[WiFi] Disconnected or lost connection.");
      break;
    default:
      break;
  }
}

static void triggerWiFiConnect() {
  WiFi.disconnect();
  delay(10);

  if (currentNetwork == 1) {
    if (activeSsidPrimary.length() == 0) {
      if (activeSsidBackup.length() > 0) {
        currentNetwork = 2;
      } else {
        return;
      }
    }
    if (currentNetwork == 1) {
#if ENABLE_STATIC_IP
      if (!WiFi.config(LOCAL_IP, GATEWAY, SUBNET, PRIMARY_DNS, SECONDARY_DNS)) {
        Serial.println("[WiFi] Failed to configure Static IP");
      }
#else
      WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
#endif
      Serial.print("[WiFi] Connecting to Primary Wi-Fi: ");
      Serial.println(activeSsidPrimary);
      WiFi.begin(activeSsidPrimary.c_str(), activePassPrimary.c_str());
    }
  }

  if (currentNetwork == 2) {
    if (activeSsidBackup.length() == 0) {
      currentNetwork = 1;
      return;
    }
    WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
    Serial.print("[WiFi] Connecting to Backup Wi-Fi: ");
    Serial.println(activeSsidBackup);
    WiFi.begin(activeSsidBackup.c_str(), activePassBackup.c_str());
  }
  lastWifiAttempt = millis();
}

void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.onEvent(onWiFiEvent);

  bool hasCredentials = loadWiFiCredentials(activeSsidPrimary, activePassPrimary, activeSsidBackup, activePassBackup);

  // Only seed compile-time credentials if NVS is empty
  if (!hasCredentials && strlen(WIFI_SSID_PRIMARY) > 0) {
    saveWiFiCredentials(WIFI_SSID_PRIMARY, WIFI_PASS_PRIMARY, WIFI_SSID_BACKUP, WIFI_PASS_BACKUP);
    hasCredentials = loadWiFiCredentials(activeSsidPrimary, activePassPrimary, activeSsidBackup, activePassBackup);
  }

  if (!hasCredentials) {
    Serial.println("[WiFi] No Wi-Fi credentials in NVS. Starting emergency setup AP...");
    startSoftAPPortal();
    return;
  }

  Serial.println("\n[WiFi] Initializing Wi-Fi Station Mode from NVS...");
  triggerWiFiConnect();

  unsigned long start = millis();
  while (!wifiConnected && (millis() - start < 7000)) {
    delay(10);
  }

  if (!wifiConnected && activeSsidBackup.length() > 0) {
    Serial.println("[WiFi] Initial connection pending in background. Switching fallback network.");
    currentNetwork = 2;
    triggerWiFiConnect();
  }
}

static unsigned long wifiOfflineStartTime = 0;

void handleWiFiReconnection() {
  if (webOtaSuspended || isFirmwareUpdating() || arduinoOtaActive) {
    return;
  }

  if (apDisablePending && wifiConnected) {
    apDisablePending = false;
    WiFi.softAPdisconnect(true);
    apModeActive = false;
    Serial.println("[WiFi] Station connected; SoftAP disabled.");
  }

  if (mdnsPending && wifiConnected) {
    mdnsPending = false;
    if (MDNS.begin(DEVICE_HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("[mDNS] Responder active: http://%s.local\n", DEVICE_HOSTNAME);
    }
  }

  if (wifiConnected) {
    wifiOfflineStartTime = 0;
  } else {
    if (wifiOfflineStartTime == 0) {
      wifiOfflineStartTime = millis();
    }
    // Fallback to emergency SoftAP if unable to connect for > 60 seconds
    if (!apModeActive && (millis() - wifiOfflineStartTime >= 60000UL)) {
      Serial.println("[WiFi] Reconnection failed for >60s. Starting emergency SoftAP portal...");
      startSoftAPPortal();
    }
  }

  if (activeSsidPrimary.length() == 0 && activeSsidBackup.length() == 0) {
    if (!apModeActive) {
      startSoftAPPortal();
    }
    return;
  }

  if (!wifiConnected && (!apModeActive || WiFi.softAPgetStationNum() == 0) && (millis() - lastWifiAttempt >= RECONNECT_INTERVAL_MS)) {
    Serial.println("[WiFi] Connection down. Triggering non-blocking reconnect...");
    if (activeSsidBackup.length() > 0) {
      currentNetwork = (currentNetwork == 1) ? 2 : 1;
    } else {
      currentNetwork = 1;
    }
    triggerWiFiConnect();
  }
}

// Confirm partition health to cancel rollback after stability window
void validateAppRollback() {
  static bool validated = false;
  if (validated) return;

  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state;
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      bool hasCredentials = (activeSsidPrimary.length() > 0 || activeSsidBackup.length() > 0);
      if (hasCredentials) {
        if (!wifiConnected && millis() > 180000UL) {
          Serial.println("[OTA] Critical: Wi-Fi unreachable after 3 minutes. Rolling back firmware!");
          esp_ota_mark_app_invalid_rollback_and_reboot();
          return;
        }
        if (!wifiConnected || (millis() - wifiConnectedSince < 30000UL)) return;
      } else {
        if (millis() < 30000UL) return;
      }

      esp_ota_mark_app_valid_cancel_rollback();
      Serial.println("[OTA] Firmware self-check passed: 30s runtime & network stable. Rollback cancelled.");
    }
  }
  validated = true;
}

void handleGetState() {
  sendReadOnlyCORSHeaders();
  char json[512];
  unsigned long uptimeSec = millis() / 1000;
  uint8_t posPct;
  DoorState curr;
  bool openSens, closedSens, obsWarn, sensFault, sensTimeout, failedMove, midStall, isCal, unseated;
  DoorState lastDir;
  unsigned long openDur, closeDur;

  {
    DoorStateLock lock;
    if (!lock.acquired) {
      server.send(503, "application/json", "{\"status\":\"error\",\"message\":\"Door controller busy\"}");
      return;
    }
    posPct = calculateCurrentPosition();
    curr = currentState;
    openSens = realOpenSensor;
    closedSens = realClosedSensor;
    obsWarn = obstacleWarning;
    sensFault = sensorFault;
    sensTimeout = sensorTimeoutError;
    failedMove = failedToMove;
    midStall = midTrackStall;
    lastDir = lastCommandedDirection;
    openDur = openDurationMs;
    closeDur = closeDurationMs;
    isCal = isCalibrated;
    unseated = switchUnseated;
  }

  snprintf(json, sizeof(json),
    "{"
      "\"state\":\"%s\","
      "\"open_sensor\":%s,"
      "\"close_sensor\":%s,"
      "\"obstacle_warning\":%s,"
      "\"sensor_fault\":%s,"
      "\"sensor_timeout_error\":%s,"
      "\"failed_to_move\":%s,"
      "\"mid_track_stall\":%s,"
      "\"last_commanded_direction\":\"%s\","
      "\"uptime_seconds\":%lu,"
      "\"position_pct\":%u,"
      "\"open_duration_ms\":%lu,"
      "\"close_duration_ms\":%lu,"
      "\"is_calibrated\":%s,"
      "\"switch_unseated\":%s"
    "}",
    getDebugString(curr),
    openSens ? "true" : "false",
    closedSens ? "true" : "false",
    obsWarn ? "true" : "false",
    sensFault ? "true" : "false",
    sensTimeout ? "true" : "false",
    failedMove ? "true" : "false",
    midStall ? "true" : "false",
    getDebugString(lastDir),
    uptimeSec,
    posPct,
    openDur,
    closeDur,
    isCal ? "true" : "false",
    unseated ? "true" : "false"
  );

  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "application/json", json);
}

static const char ROOT_PAGE_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Garage Door Controller</title>
<style>
:root{
  --bg:#070b14;--card:#0f172a;--card-border:#1e293b;--text:#f8fafc;--muted:#94a3b8;
  --accent:#38bdf8;--accent-glow:rgba(56,189,248,0.25);
  --success:#10b981;--success-glow:rgba(16,185,129,0.25);
  --warning:#f59e0b;--warning-glow:rgba(245,158,11,0.25);
  --danger:#ef4444;--danger-glow:rgba(239,68,68,0.25);
  --indigo:#6366f1;--indigo-glow:rgba(99,102,241,0.25);
}
*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
body{background:var(--bg);color:var(--text);min-height:100vh;padding:16px;display:flex;flex-direction:column;align-items:center}
.container{width:100%;max-width:1040px;display:flex;flex-direction:column;gap:16px}
header{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:14px;background:var(--card);border:1px solid var(--card-border);border-radius:14px;padding:16px 20px;box-shadow:0 10px 30px rgba(0,0,0,0.5)}
.brand{display:flex;align-items:center;gap:12px}
.brand-icon{width:44px;height:44px;border-radius:10px;background:linear-gradient(135deg,#0284c7,#2563eb);display:flex;align-items:center;justify-content:center;box-shadow:0 0 16px var(--accent-glow);flex-shrink:0}
.brand h1{font-size:1.25rem;font-weight:700;letter-spacing:-0.01em;color:#f8fafc}
.brand-meta{display:flex;align-items:center;flex-wrap:wrap;gap:8px;font-size:0.8rem;color:var(--muted);margin-top:3px}
.status-pill{display:inline-flex;align-items:center;gap:6px;padding:3px 9px;border-radius:99px;font-size:0.75rem;font-weight:600}
.status-pill.online{background:rgba(16,185,129,0.15);color:#34d399;border:1px solid rgba(16,185,129,0.3)}
.status-pill.offline{background:rgba(239,68,68,0.15);color:#f87171;border:1px solid rgba(239,68,68,0.3)}
.dot{width:7px;height:7px;border-radius:50%;background:currentColor;display:inline-block}
.nav-actions{display:flex;flex-wrap:wrap;gap:8px}
.btn{display:inline-flex;align-items:center;gap:7px;padding:9px 15px;border-radius:8px;font-size:0.85rem;font-weight:600;text-decoration:none;cursor:pointer;border:1px solid transparent;transition:all .15s ease}
.btn-primary{background:#0284c7;color:#fff}
.btn-primary:hover{background:#0369a1;box-shadow:0 0 14px var(--accent-glow)}
.btn-ghost{background:rgba(255,255,255,0.05);color:var(--text);border-color:var(--card-border)}
.btn-ghost:hover{background:rgba(255,255,255,0.09);border-color:#475569}
.btn-danger{background:rgba(239,68,68,0.12);color:#fca5a5;border-color:rgba(239,68,68,0.25)}
.btn-danger:hover{background:#ef4444;color:#fff;box-shadow:0 0 14px var(--danger-glow)}
.grid-main{display:grid;grid-template-columns:1.15fr 0.85fr;gap:16px}
@media(max-width:880px){.grid-main{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--card-border);border-radius:14px;padding:20px;box-shadow:0 6px 25px rgba(0,0,0,0.35);display:flex;flex-direction:column;gap:16px}
.card-title{font-size:0.85rem;font-weight:700;text-transform:uppercase;letter-spacing:0.06em;color:var(--muted);display:flex;justify-content:space-between;align-items:center}
.door-hero{display:flex;flex-direction:column;align-items:center;padding:12px 0 6px 0;text-align:center;gap:14px}
.state-badge{font-size:1.6rem;font-weight:800;letter-spacing:0.04em;padding:8px 26px;border-radius:12px;border:1px solid transparent;transition:all .25s ease;display:inline-block}
.state-CLOSED{background:rgba(16,185,129,0.15);color:#34d399;border-color:rgba(16,185,129,0.35);box-shadow:0 0 20px var(--success-glow)}
.state-OPEN{background:rgba(56,189,248,0.15);color:#38bdf8;border-color:rgba(56,189,248,0.35);box-shadow:0 0 20px var(--accent-glow)}
.state-OPENING{background:rgba(99,102,241,0.2);color:#a5b4fc;border-color:rgba(99,102,241,0.45);box-shadow:0 0 20px var(--indigo-glow);animation:pulse 1.3s infinite}
.state-CLOSING{background:rgba(245,158,11,0.2);color:#fbbf24;border-color:rgba(245,158,11,0.45);box-shadow:0 0 20px var(--warning-glow);animation:pulse 1.3s infinite}
.state-STOPPED{background:rgba(239,68,68,0.15);color:#f87171;border-color:rgba(239,68,68,0.35);box-shadow:0 0 15px var(--danger-glow)}
.state-UNKNOWN{background:rgba(148,163,184,0.15);color:#cbd5e1;border-color:rgba(148,163,184,0.35)}
@keyframes pulse{0%,100%{opacity:1;transform:scale(1)}50%{opacity:0.85;transform:scale(0.985)}}
.door-graphic-box{width:200px;height:130px;background:#050912;border:3px solid #334155;border-radius:8px;position:relative;overflow:hidden}
.door-slats{position:absolute;top:0;left:0;right:0;bottom:0;background:repeating-linear-gradient(0deg,#1e293b,#1e293b 10px,#0b1222 10px,#0b1222 20px);border-bottom:4px solid #64748b;will-change:transform}
.door-handle{position:absolute;bottom:8px;left:50%;transform:translateX(-50%);width:32px;height:6px;background:#94a3b8;border-radius:3px}
.door-controls{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;width:100%}
.btn-action{padding:14px 10px;font-size:0.95rem;font-weight:700;border-radius:10px;border:none;cursor:pointer;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px;transition:all .15s ease}
.btn-action:disabled{opacity:0.4;cursor:not-allowed}
.btn-open{background:#059669;color:#fff}
.btn-open:hover:not(:disabled){background:#10b981;box-shadow:0 0 16px var(--success-glow)}
.btn-close{background:#d97706;color:#fff}
.btn-close:hover:not(:disabled){background:#f59e0b;box-shadow:0 0 16px var(--warning-glow)}
.btn-toggle{background:#4f46e5;color:#fff}
.btn-toggle:hover:not(:disabled){background:#6366f1;box-shadow:0 0 16px var(--indigo-glow)}
.lockout-bar{width:100%;height:4px;background:#1e293b;border-radius:2px;overflow:hidden}
.lockout-progress{height:100%;background:var(--accent);width:0%;transition:width 0.1s linear}
.telemetry-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.tele-item{background:#080d1a;border:1px solid #1e293b;border-radius:8px;padding:10px 12px;display:flex;flex-direction:column;gap:3px}
.tele-item.clickable{cursor:pointer;transition:all .15s ease;user-select:none}
.tele-item.clickable:hover{border-color:var(--accent);background:#0d1527;box-shadow:0 0 12px var(--accent-glow);transform:translateY(-1px)}
.tele-item.clickable:active{transform:translateY(0)}
.tele-label{font-size:0.75rem;color:var(--muted);font-weight:600;text-transform:uppercase;letter-spacing:0.03em}
.tele-val{font-size:0.88rem;font-weight:700;display:flex;align-items:center;gap:6px}
.tele-val.active{color:#34d399}
.tele-val.inactive{color:#94a3b8}
.tele-val.alert{color:#f87171}
.console-card{grid-column:1/-1}
.console-header{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px}
.api-btns{display:flex;flex-wrap:wrap;gap:8px}
.btn-api{padding:6px 12px;font-size:0.8rem;font-weight:600;border-radius:6px;background:#1e293b;color:#cbd5e1;border:1px solid #334155;cursor:pointer;transition:all .15s}
.btn-api:hover{background:#334155;color:#fff;border-color:#475569}
.console-log{background:#040711;border:1px solid #1e293b;border-radius:8px;padding:12px;font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace;font-size:0.8rem;color:#cbd5e1;max-height:160px;overflow-y:auto;display:flex;flex-direction:column;gap:6px}
.log-line{display:flex;gap:10px;line-height:1.4}
.log-time{color:var(--muted);flex-shrink:0}
.log-ok{color:#34d399}
.log-err{color:#f87171}
.toast{position:fixed;bottom:24px;right:24px;background:#1e293b;border:1px solid var(--accent);color:var(--text);padding:12px 18px;border-radius:8px;box-shadow:0 12px 30px rgba(0,0,0,0.7);font-size:0.9rem;display:none;align-items:center;gap:8px;z-index:9999}
.quick-info{background:#080d1a;border:1px solid #1e293b;border-radius:8px;padding:10px 14px;font-size:0.8rem;color:var(--muted);display:flex;flex-wrap:wrap;justify-content:space-between;gap:8px}
</style>
</head>
<body>
<div class="container">
  <header>
    <div class="brand">
      <div class="brand-icon">
        <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#fff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 9l9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"></path><polyline points="9 22 9 12 15 12 15 22"></polyline></svg>
      </div>
      <div>
        <h1>ESP32 Garage Door</h1>
        <div class="brand-meta">
          <span>IP: <strong id="hostIpDisplay">--</strong></span> &bull;
          <span>garage-door.local</span> &bull;
          <span id="connPill" class="status-pill online"><span class="dot"></span><span id="connText">Online</span></span>
        </div>
      </div>
    </div>
    <div class="nav-actions">
      <a href="/update" class="btn btn-primary" title="Over-the-Air Firmware Update">
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"></path><polyline points="17 8 12 3 7 8"></polyline><line x1="12" y1="3" x2="12" y2="15"></line></svg>
        OTA Update
      </a>
      <a href="/setup" class="btn btn-ghost" title="Wi-Fi SSID and Password Configuration">
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"></circle><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"></path></svg>
        Wi-Fi Setup
      </a>
      <a href="/state" target="_blank" class="btn btn-ghost" title="Direct JSON REST State Endpoint">
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"></path><polyline points="14 2 14 8 20 8"></polyline><line x1="16" y1="13" x2="8" y2="13"></line><line x1="16" y1="17" x2="8" y2="17"></line><polyline points="10 9 9 9 8 9"></polyline></svg>
        GET /state
      </a>
      <button onclick="rebootEsp()" class="btn btn-danger" title="Reboot ESP32 Microcontroller">
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"></polyline><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"></path></svg>
        Reboot
      </button>
    </div>
  </header>

  <div class="grid-main">
    <!-- DOOR CONTROLLER HERO -->
    <div class="card">
      <div class="card-title">
        <span>Door Status & Controls</span>
        <span id="lastUpdated" style="font-size:0.75rem;text-transform:none;color:var(--muted)">Updating...</span>
      </div>

      <div class="door-hero">
        <div class="door-graphic-box">
          <div id="doorTrack" class="door-slats" style="transform:translateY(0%)">
            <div class="door-handle"></div>
          </div>
        </div>
        <div id="doorStateBadge" class="state-badge state-UNKNOWN">UNKNOWN</div>
        <div id="doorSubtext" style="font-size:0.85rem;color:var(--muted)">Polling controller status...</div>
      </div>

      <div class="door-controls">
        <button id="btnOpen" onclick="sendCmd('/on','OPEN')" class="btn-action btn-open" title="Send POST /on (Open Door)">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><polyline points="18 15 12 9 6 15"></polyline></svg>
          OPEN
        </button>
        <button id="btnClose" onclick="sendCmd('/off','CLOSE')" class="btn-action btn-close" title="Send POST /off (Close Door)">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><polyline points="6 9 12 15 18 9"></polyline></svg>
          CLOSE
        </button>
        <button id="btnToggle" onclick="sendCmd('/toggle','TOGGLE')" class="btn-action btn-toggle" title="Send POST /toggle (Wall Switch Pulse)">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"></path></svg>
          TOGGLE
        </button>
      </div>
      <div class="lockout-bar"><div id="lockoutProgress" class="lockout-progress"></div></div>
    </div>

    <!-- SENSORS & DIAGNOSTICS -->
    <div class="card">
      <div class="card-title">
        <span>Hardware Sensors & Safety</span>
        <button onclick="fetchState()" class="btn btn-ghost" style="padding:3px 8px;font-size:0.75rem">Refresh</button>
      </div>

      <div class="telemetry-grid">
        <div class="tele-item">
          <span class="tele-label">Closed Sensor (GPIO 26)</span>
          <span id="sensorClosed" class="tele-val inactive">&bull; INACTIVE</span>
        </div>
        <div class="tele-item">
          <span class="tele-label">Open Sensor (GPIO 25)</span>
          <span id="sensorOpen" class="tele-val inactive">&bull; INACTIVE</span>
        </div>
        <div class="tele-item">
          <span class="tele-label">Obstacle Warning</span>
          <span id="flagObstacle" class="tele-val inactive">CLEAR</span>
        </div>
        <div class="tele-item">
          <span class="tele-label">Sensor Fault</span>
          <span id="flagSensorFault" class="tele-val inactive">CLEAR</span>
        </div>
        <div class="tele-item">
          <span class="tele-label">Mid-Track Stall</span>
          <span id="flagStall" class="tele-val inactive">CLEAR</span>
        </div>
        <div class="tele-item">
          <span class="tele-label">Failed To Move</span>
          <span id="flagFailedToMove" class="tele-val inactive">CLEAR</span>
        </div>
        <div class="tele-item clickable" onclick="sendCmd('/calibrate/reset','POST')" title="Click to reset calibration & daily quota immediately">
          <div style="display:flex;justify-content:space-between;align-items:center">
            <span class="tele-label">Calibrated Flight</span>
            <span style="font-size:0.68rem;color:var(--accent);font-weight:700;letter-spacing:0.02em">RESET ↺</span>
          </div>
          <span id="calFlight" class="tele-val">17.0s / 17.0s</span>
        </div>
        <div class="tele-item">
          <span class="tele-label">Estimated Position</span>
          <span id="posValue" class="tele-val">0%</span>
        </div>
        <div class="tele-item">
          <span class="tele-label">Last Commanded</span>
          <span id="lastDirection" class="tele-val">NONE</span>
        </div>
        <div class="tele-item">
          <span class="tele-label">Controller Uptime</span>
          <span id="uptimeVal" class="tele-val">0s</span>
        </div>
      </div>

      <div class="quick-info">
        <span>ArduinoOTA Port: <strong>3232</strong></span>
        <span>mDNS: <strong>garage-door.local</strong></span>
        <span>Relay Pin: <strong>GPIO 27</strong></span>
      </div>
    </div>

    <!-- REST API CONSOLE -->
    <div class="card console-card">
      <div class="console-header">
        <div class="card-title" style="margin:0">HTTP REST API Request Sandbox</div>
        <div class="api-btns">
          <button class="btn-api" onclick="sendCmd('/state','GET')">GET /state</button>
          <button class="btn-api" onclick="sendCmd('/toggle','POST')">POST /toggle</button>
          <button class="btn-api" onclick="sendCmd('/on','POST')">POST /on</button>
          <button class="btn-api" onclick="sendCmd('/off','POST')">POST /off</button>
          <button class="btn-api" onclick="sendCmd('/calibrate/reset','POST')">POST /calibrate/reset</button>
          <button class="btn-api" onclick="sendCmd('/reboot','POST')">POST /reboot</button>
          <button class="btn-api" onclick="clearLog()">Clear Log</button>
        </div>
      </div>
      <div id="consoleLog" class="console-log">
        <div class="log-line"><span class="log-time">[Ready]</span><span>Dashboard active. Polling /state every 2s.</span></div>
      </div>
    </div>
  </div>
</div>

<div id="toast" class="toast"></div>

<script>
let lastState = null;

function log(msg, isErr=false){
  const c = document.getElementById('consoleLog');
  const d = new Date().toTimeString().split(' ')[0];
  const div = document.createElement('div');
  div.className = 'log-line';
  div.innerHTML = `<span class="log-time">[${d}]</span><span class="${isErr ? 'log-err':'log-ok'}">${msg}</span>`;
  c.prepend(div);
}

function clearLog(){
  document.getElementById('consoleLog').innerHTML = '';
}

function showToast(text, ms=2500){
  const t = document.getElementById('toast');
  t.textContent = text;
  t.style.display = 'flex';
  clearTimeout(t.timer);
  t.timer = setTimeout(()=>{ t.style.display='none'; }, ms);
}

function formatUptime(sec){
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  if (d > 0) return `${d}d ${h}h ${m}m`;
  if (h > 0) return `${h}h ${m}m ${s}s`;
  return `${m}m ${s}s`;
}

let isFirstLoad = true;
let clientPosPct = 0;
let rafId = null;
let animStartTime = null;
let animStartPos = 0;
let animTargetPos = 0;
let animDurationMs = 17000;
let animState = null;
let pollTimeout = null;
let openDurationMs = 17000;
let closeDurationMs = 17000;
let lastCommandedDir = 'NONE';

function renderPosition(pct) {
  clientPosPct = Math.max(0, Math.min(100, pct));
  const track = document.getElementById('doorTrack');
  if (track) {
    track.style.transition = 'none';
    track.style.transform = `translateY(-${(clientPosPct * 0.85).toFixed(2)}%)`;
  }
  const posEl = document.getElementById('posValue');
  if (posEl) posEl.textContent = `${Math.round(clientPosPct)}%`;
  
  const b = document.getElementById('doorStateBadge');
  if (b && animState) {
    b.textContent = `${animState} (${Math.round(clientPosPct)}%)`;
  }
}

function startClientAnimation(targetState, startPct, targetPct, durationMs) {
  if (rafId) {
    cancelAnimationFrame(rafId);
    rafId = null;
  }
  animState = targetState;
  animStartPos = startPct;
  animTargetPos = targetPct;
  animDurationMs = Math.max(300, durationMs);
  animStartTime = performance.now();

  function step(now) {
    const elapsed = now - animStartTime;
    const progress = Math.min(1, elapsed / animDurationMs);
    const currentPos = animStartPos + (animTargetPos - animStartPos) * progress;
    renderPosition(currentPos);

    if (progress < 1 && animState === targetState) {
      rafId = requestAnimationFrame(step);
    } else {
      rafId = null;
      const finishedState = animState;
      animState = null;
      renderPosition(animTargetPos);
      const b = document.getElementById('doorStateBadge');
      if (b) {
        if (finishedState === 'OPENING' && animTargetPos >= 100) {
          b.className = 'state-badge state-OPEN';
          b.textContent = 'OPEN';
        } else if (finishedState === 'CLOSING' && animTargetPos <= 0) {
          b.className = 'state-badge state-CLOSED';
          b.textContent = 'CLOSED';
        } else if (finishedState === 'STOPPED') {
          b.className = 'state-badge state-STOPPED';
          b.textContent = `STOPPED (${Math.round(animTargetPos)}%)`;
        }
      }
    }
  }

  rafId = requestAnimationFrame(step);
}

function stopClientAnimation(finalPct) {
  if (rafId) {
    cancelAnimationFrame(rafId);
    rafId = null;
  }
  animState = null;
  if (finalPct !== null && finalPct !== undefined) {
    renderPosition(finalPct);
  }
}

function handleDirectReaction(path) {
  if (path === '/on') {
    if (animState !== 'OPENING' && clientPosPct < 100) {
      const remPct = Math.max(0, 100 - clientPosPct);
      const remDur = Math.round((openDurationMs * remPct) / 100);
      const b = document.getElementById('doorStateBadge');
      if (b) {
        b.className = 'state-badge state-OPENING';
        b.textContent = `OPENING (${Math.round(clientPosPct)}%)`;
      }
      lastCommandedDir = 'OPENING';
      startClientAnimation('OPENING', clientPosPct, 100, remDur);
    }
  } else if (path === '/off') {
    if (animState !== 'CLOSING' && clientPosPct > 0) {
      const remPct = Math.max(0, clientPosPct);
      const remDur = Math.round((closeDurationMs * remPct) / 100);
      const b = document.getElementById('doorStateBadge');
      if (b) {
        b.className = 'state-badge state-CLOSING';
        b.textContent = `CLOSING (${Math.round(clientPosPct)}%)`;
      }
      lastCommandedDir = 'CLOSING';
      startClientAnimation('CLOSING', clientPosPct, 0, remDur);
    }
  } else if (path === '/toggle') {
    if (animState === 'OPENING' || animState === 'CLOSING') {
      lastCommandedDir = animState;
      stopClientAnimation(clientPosPct);
      const b = document.getElementById('doorStateBadge');
      if (b) {
        b.className = 'state-badge state-STOPPED';
        b.textContent = `STOPPED (${Math.round(clientPosPct)}%)`;
      }
    } else if (clientPosPct <= 0 || lastState === 'CLOSED') {
      const remPct = Math.max(0, 100 - clientPosPct);
      const remDur = Math.round((openDurationMs * remPct) / 100);
      const b = document.getElementById('doorStateBadge');
      if (b) {
        b.className = 'state-badge state-OPENING';
        b.textContent = `OPENING (${Math.round(clientPosPct)}%)`;
      }
      lastCommandedDir = 'OPENING';
      startClientAnimation('OPENING', clientPosPct, 100, remDur);
    } else if (clientPosPct >= 100 || lastState === 'OPEN') {
      const remPct = Math.max(0, clientPosPct);
      const remDur = Math.round((closeDurationMs * remPct) / 100);
      const b = document.getElementById('doorStateBadge');
      if (b) {
        b.className = 'state-badge state-CLOSING';
        b.textContent = `CLOSING (${Math.round(clientPosPct)}%)`;
      }
      lastCommandedDir = 'CLOSING';
      startClientAnimation('CLOSING', clientPosPct, 0, remDur);
    } else {
      if (lastCommandedDir === 'OPENING') {
        const remPct = Math.max(0, clientPosPct);
        const remDur = Math.round((closeDurationMs * remPct) / 100);
        const b = document.getElementById('doorStateBadge');
        if (b) {
          b.className = 'state-badge state-CLOSING';
          b.textContent = `CLOSING (${Math.round(clientPosPct)}%)`;
        }
        lastCommandedDir = 'CLOSING';
        startClientAnimation('CLOSING', clientPosPct, 0, remDur);
      } else {
        const remPct = Math.max(0, 100 - clientPosPct);
        const remDur = Math.round((openDurationMs * remPct) / 100);
        const b = document.getElementById('doorStateBadge');
        if (b) {
          b.className = 'state-badge state-OPENING';
          b.textContent = `OPENING (${Math.round(clientPosPct)}%)`;
        }
        lastCommandedDir = 'OPENING';
        startClientAnimation('OPENING', clientPosPct, 100, remDur);
      }
    }
  }
}

function updateUI(data){
  const state = (data.state || '').toUpperCase();
  lastState = state;
  document.getElementById('lastUpdated').textContent = 'Live • ' + new Date().toLocaleTimeString();

  if (data.last_commanded_direction) {
    lastCommandedDir = data.last_commanded_direction.toUpperCase();
  }

  if (typeof data.open_duration_ms === 'number' && data.open_duration_ms >= 5000) {
    openDurationMs = data.open_duration_ms;
  }
  if (typeof data.close_duration_ms === 'number' && data.close_duration_ms >= 5000) {
    closeDurationMs = data.close_duration_ms;
  }

  const serverPos = (data.position_pct !== undefined) ? data.position_pct : clientPosPct;

  const b = document.getElementById('doorStateBadge');

  if (isFirstLoad) {
    if (state === 'OPEN') {
      stopClientAnimation(100);
    } else if (state === 'CLOSED') {
      stopClientAnimation(0);
    } else if (state === 'OPENING') {
      const startPct = (serverPos < 100) ? serverPos : 0;
      const remDur = Math.round(((100 - startPct) / 100) * openDurationMs);
      startClientAnimation('OPENING', startPct, 100, remDur);
    } else if (state === 'CLOSING') {
      const startPct = (serverPos > 0) ? serverPos : 100;
      const remDur = Math.round((startPct / 100) * closeDurationMs);
      startClientAnimation('CLOSING', startPct, 0, remDur);
    } else {
      stopClientAnimation(serverPos);
    }
    isFirstLoad = false;
  } else {
    if (state === 'OPEN') {
      const diff = Math.abs(clientPosPct - 100);
      if (animState === 'OPENING') {
        if (diff > 5) {
          startClientAnimation('OPENING', clientPosPct, 100, 400);
        }
      } else {
        if (diff > 5) {
          startClientAnimation('OPENING', clientPosPct, 100, 400);
        } else {
          stopClientAnimation(100);
        }
      }
    } else if (state === 'CLOSED') {
      const diff = Math.abs(clientPosPct - 0);
      if (animState === 'CLOSING') {
        if (diff > 5) {
          startClientAnimation('CLOSING', clientPosPct, 0, 400);
        }
      } else {
        if (diff > 5) {
          startClientAnimation('CLOSING', clientPosPct, 0, 400);
        } else {
          stopClientAnimation(0);
        }
      }
    } else if (state === 'STOPPED') {
      const diff = Math.abs(clientPosPct - serverPos);
      if (diff > 5) {
        startClientAnimation('STOPPED', clientPosPct, serverPos, 500);
      } else {
        if (animState !== null) {
          stopClientAnimation(clientPosPct);
        }
      }
    } else if (state === 'OPENING') {
      const diff = Math.abs(clientPosPct - serverPos);
      const remDur = Math.max(800, Math.round(((100 - serverPos) / 100) * openDurationMs));
      if (animState === 'OPENING') {
        if (diff > 5) {
          startClientAnimation('OPENING', clientPosPct, 100, remDur);
        }
      } else {
        startClientAnimation('OPENING', clientPosPct, 100, remDur);
      }
    } else if (state === 'CLOSING') {
      const diff = Math.abs(clientPosPct - serverPos);
      const remDur = Math.max(800, Math.round((serverPos / 100) * closeDurationMs));
      if (animState === 'CLOSING') {
        if (diff > 5) {
          startClientAnimation('CLOSING', clientPosPct, 0, remDur);
        }
      } else {
        startClientAnimation('CLOSING', clientPosPct, 0, remDur);
      }
    }
  }

  if (b) {
    b.className = 'state-badge state-' + state;
    if (animState === null) {
      if (state === 'OPEN') b.textContent = 'OPEN';
      else if (state === 'CLOSED') b.textContent = 'CLOSED';
      else if (state === 'STOPPED') b.textContent = `STOPPED (${Math.round(clientPosPct)}%)`;
      else b.textContent = `${state} (${Math.round(clientPosPct)}%)`;
    }
  }

  document.getElementById('doorSubtext').textContent =
    'Last direction: ' + (data.last_commanded_direction || 'NONE');

  const sClosed = document.getElementById('sensorClosed');
  sClosed.className = 'tele-val ' + (data.close_sensor ? 'active' : 'inactive');
  sClosed.innerHTML = data.close_sensor ? '&bull; ACTIVE (CLOSED)' : '&bull; INACTIVE';

  const sOpen = document.getElementById('sensorOpen');
  sOpen.className = 'tele-val ' + (data.open_sensor ? 'active' : 'inactive');
  sOpen.innerHTML = data.open_sensor ? '&bull; ACTIVE (OPEN)' : '&bull; INACTIVE';

  function setFlag(elId, val, alertText='ALERT', okText='CLEAR'){
    const el = document.getElementById(elId);
    if(el) {
      el.className = 'tele-val ' + (val ? 'alert' : 'inactive');
      el.textContent = val ? alertText : okText;
    }
  }
  setFlag('flagObstacle', data.obstacle_warning, 'DETECTED');
  setFlag('flagSensorFault', data.sensor_fault, 'FAULT');
  setFlag('flagStall', data.mid_track_stall, 'STALLED');
  setFlag('flagFailedToMove', data.failed_to_move, 'STUCK');

  const openSec = (openDurationMs / 1000).toFixed(1);
  const closeSec = (closeDurationMs / 1000).toFixed(1);
  const calBadge = data.is_calibrated ? ' (Calibrated)' : ' (Default)';
  const calEl = document.getElementById('calFlight');
  if(calEl) calEl.textContent = `${openSec}s / ${closeSec}s${calBadge}`;

  if (animState === null) {
    const posEl = document.getElementById('posValue');
    if (posEl) posEl.textContent = `${Math.round(clientPosPct)}%`;
  }

  document.getElementById('lastDirection').textContent = data.last_commanded_direction || 'NONE';
  document.getElementById('uptimeVal').textContent = formatUptime(data.uptime_seconds || 0);

  const p = document.getElementById('connPill');
  p.className = 'status-pill online';
  document.getElementById('connText').textContent = 'Online';
}

function startLockout(){
  const bar = document.getElementById('lockoutProgress');
  bar.style.transition = 'none';
  bar.style.width = '100%';
  setTimeout(()=>{
    bar.style.transition = 'width 1.5s linear';
    bar.style.width = '0%';
  }, 20);
}

async function fetchState(){
  clearTimeout(pollTimeout);
  try{
    const res = await fetch('/state');
    if(!res.ok) throw new Error('HTTP ' + res.status);
    const data = await res.json();
    updateUI(data);

    if (document.visibilityState === 'visible') {
      pollTimeout = setTimeout(fetchState, 2000);
    }
  }catch(e){
    const p = document.getElementById('connPill');
    p.className = 'status-pill offline';
    document.getElementById('connText').textContent = 'Offline';
    if (document.visibilityState === 'visible') {
      pollTimeout = setTimeout(fetchState, 3000);
    }
  }
}

async function sendCmd(path, methodOrLabel){
  startLockout();
  handleDirectReaction(path);
  const method = (path === '/state') ? 'GET' : 'POST';
  try{
    log(`Sending ${method} ${path}...`);
    const res = await fetch(path, {method: method});
    const text = await res.text();
    let json = null;
    try { json = JSON.parse(text); } catch(_) {}
    const msg = json ? (json.message || JSON.stringify(json)) : text;
    log(`${method} ${path} -> [${res.status}] ${msg}`, !res.ok);
    showToast(msg);
    setTimeout(fetchState, 150);
  }catch(e){
    log(`${method} ${path} FAILED: ${e.message}`, true);
    showToast('Request Failed: ' + e.message);
  }
}

async function rebootEsp(){
  if(!confirm('Reboot the ESP32 garage door controller now?')) return;
  try{
    await fetch('/reboot', {method:'POST'});
    showToast('Rebooting ESP32... Please wait 5s.');
    log('POST /reboot -> Controller restarting...');
  }catch(e){
    showToast('Reboot trigger sent.');
  }
}

document.addEventListener('visibilitychange', ()=>{
  if(document.visibilityState === 'visible') fetchState();
  else clearTimeout(pollTimeout);
});

const hostEl = document.getElementById('hostIpDisplay');
if (hostEl) {
  hostEl.textContent = window.location.hostname || '192.168.1.33';
}

fetchState();
</script>
</body>
</html>)rawliteral";

void handleRoot() {
  if (apModeActive && !wifiConnected && activeSsidPrimary.length() == 0 && activeSsidBackup.length() == 0) {
    handleSetupForm();
    return;
  }
  sendCORSHeaders();
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send_P(200, "text/html", ROOT_PAGE_HTML);
}

void handleReboot() {
  if (!isSameOriginRequest()) {
    server.send(403, "application/json", "{\"status\":\"error\",\"message\":\"Cross-origin request forbidden\"}");
    return;
  }
  if (isFirmwareUpdating()) {
    server.send(409, "application/json", "{\"status\":\"error\",\"message\":\"Firmware update in progress. Reboot locked.\"}");
    return;
  }
  {
    DoorStateLock lock;
    if (!lock.acquired) {
      server.send(503, "application/json", "{\"status\":\"error\",\"message\":\"Door controller busy\"}");
      return;
    }
    if (currentState == STATE_OPENING || currentState == STATE_CLOSING) {
      server.send(409, "application/json", "{\"status\":\"error\",\"message\":\"Door is in motion. Reboot locked for safety.\"}");
      return;
    }
    suspendSafetyTask();
  }
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Rebooting ESP32 controller...\"}");
  delay(1000);
  ESP.restart();
}

void handleNotFound() {
  if (apModeActive && !wifiConnected && activeSsidPrimary.length() == 0 && activeSsidBackup.length() == 0) {
    handleSetupForm();
    return;
  }
  sendCORSHeaders();
  server.send(404, "application/json", "{\"error\":\"Endpoint or Method not found. Available: GET /, /state, /update, /setup | POST /toggle, /on, /off, /reboot, /update, /setup, /calibrate/reset.\"}");
}

static const char SETUP_PAGE_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Garage Door Controller - Wi-Fi Setup</title>
<style>
body{font-family:system-ui,-apple-system,sans-serif;background:#0f172a;color:#e2e8f0;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0;padding:20px;box-sizing:border-box}
.card{background:#1e293b;border:1px solid #334155;border-radius:12px;padding:24px;width:100%;max-width:400px;box-shadow:0 10px 25px rgba(0,0,0,0.5)}
h2{margin-top:0;font-size:1.3rem;color:#38bdf8;text-align:center}
p{font-size:0.875rem;color:#94a3b8;line-height:1.4}
label{display:block;margin-top:14px;font-size:0.85rem;color:#cbd5e1;font-weight:600}
input{width:100%;box-sizing:border-box;background:#0f172a;border:1px solid #475569;border-radius:6px;padding:10px 12px;color:#f8fafc;font-size:0.95rem;margin-top:6px}
input:focus{outline:none;border-color:#38bdf8}
button{width:100%;margin-top:20px;padding:12px;border:none;border-radius:6px;background:#0284c7;color:#fff;font-size:1rem;font-weight:bold;cursor:pointer;transition:background 0.2s}
button:hover{background:#0369a1}
</style></head><body>
<div class='card'>
<h2>Wi-Fi Setup</h2>
<p>Enter network credentials for the garage door controller. Credentials will be safely stored in NVS Flash.</p>
<form method='POST' action='/setup'>
<label>Primary Wi-Fi SSID</label><input type='text' name='ssid' required>
<label>Primary Wi-Fi Password</label><input type='password' name='pass'>
<label>Backup Wi-Fi SSID (Optional)</label><input type='text' name='bak_ssid'>
<label>Backup Wi-Fi Password (Optional)</label><input type='password' name='bak_pass'>
<button type='submit'>Save & Connect</button>
</form>
<div style='margin-top:16px;text-align:center'><a href='/' style='color:#38bdf8;text-decoration:none;font-size:0.875rem'>&larr; Back to Dashboard</a></div>
</div></body></html>)rawliteral";

void handleSetupForm() {
  server.send_P(200, "text/html", SETUP_PAGE_HTML);
}

void handleSetupSave() {
  if (!isSameOriginRequest()) {
    server.send(403, "application/json", "{\"status\":\"error\",\"message\":\"Cross-origin request forbidden\"}");
    return;
  }
  if (isFirmwareUpdating()) {
    server.send(409, "application/json", "{\"status\":\"error\",\"message\":\"Firmware update in progress. Setup locked.\"}");
    return;
  }

  String newSsid = server.arg("ssid");
  String newPass = server.arg("pass");
  String newBakSsid = server.arg("bak_ssid");
  String newBakPass = server.arg("bak_pass");

  if (newSsid.length() == 0) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"SSID cannot be empty\"}");
    return;
  }

  {
    DoorStateLock lock;
    if (!lock.acquired) {
      server.send(503, "application/json", "{\"status\":\"error\",\"message\":\"Door controller busy\"}");
      return;
    }
    if (currentState == STATE_OPENING || currentState == STATE_CLOSING) {
      server.send(409, "application/json", "{\"status\":\"error\",\"message\":\"Door is in motion. Setup locked for safety.\"}");
      return;
    }
    suspendSafetyTask();
  }

  saveWiFiCredentials(newSsid.c_str(), newPass.c_str(), newBakSsid.c_str(), newBakPass.c_str());

  String resp = F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Saved</title><style>body{background:#0f172a;color:#e2e8f0;font-family:sans-serif;text-align:center;padding:50px}</style></head><body>"
    "<h2 style='color:#4ade80'>Credentials Saved!</h2>"
    "<p>The controller is restarting to connect to your Wi-Fi network...</p></body></html>");
  server.send(200, "text/html", resp);

  delay(1000);
  ESP.restart();
}

void handleUpdateForm() {
  if (isFirmwareUpdating()) {
    server.send(409, "text/plain", "409 Conflict: Firmware update already in progress.");
    return;
  }
  {
    DoorStateLock lock;
    if (!lock.acquired) {
      server.send(503, "text/plain", "503 Busy: State lock acquisition timeout.");
      return;
    }
    if (currentState == STATE_OPENING || currentState == STATE_CLOSING) {
      server.send(409, "text/plain", "409 Conflict: Door is moving. Firmware update is locked for physical safety.");
      return;
    }
  }

  static const uint32_t UPDATE_PAGE_GZ_LEN = 1209;
  static const uint8_t UPDATE_PAGE_GZ[] PROGMEM = {
    0x1f, 0x8b, 0x08, 0x00, 0xc3, 0xee, 0x9a, 0x6a, 0x02, 0xff, 0x75, 0x56, 0xdb, 0x8e, 0xdb, 0x36,
    0x10, 0xfd, 0x15, 0x06, 0x8b, 0x42, 0x36, 0xb2, 0x92, 0xef, 0x1b, 0xaf, 0x2e, 0x06, 0x92, 0x6c,
    0xb6, 0x0d, 0xd0, 0x22, 0x8b, 0xec, 0x06, 0x6d, 0x51, 0x14, 0x05, 0x25, 0x0e, 0x6d, 0x36, 0x12,
    0xa9, 0x90, 0x94, 0x2f, 0x35, 0x0c, 0xe4, 0xa5, 0x7d, 0x2a, 0xd0, 0x3e, 0xf4, 0xa9, 0x68, 0xd1,
    0x8f, 0xe8, 0x1f, 0xe5, 0x0b, 0xfa, 0x09, 0x1d, 0x51, 0xb2, 0xd7, 0x1b, 0x23, 0x36, 0x60, 0x9b,
    0x22, 0x67, 0xe6, 0x9c, 0x33, 0x87, 0xb3, 0x1b, 0x3f, 0xba, 0x7a, 0xf5, 0xfc, 0xee, 0xdb, 0x9b,
    0x17, 0x64, 0x61, 0x8b, 0x7c, 0x16, 0xb7, 0x9f, 0x40, 0xd9, 0x2c, 0x2e, 0xc0, 0x52, 0x22, 0x69,
    0x01, 0x89, 0xb7, 0x14, 0xb0, 0x2a, 0x95, 0xb6, 0x1e, 0xc9, 0x94, 0xb4, 0x20, 0x6d, 0xe2, 0xad,
    0x04, 0xb3, 0x8b, 0x84, 0xc1, 0x52, 0x64, 0xe0, 0xbb, 0xc5, 0xb9, 0x90, 0xc2, 0x0a, 0x9a, 0xfb,
    0x26, 0xa3, 0x39, 0x24, 0x03, 0x6f, 0x16, 0x5b, 0x61, 0x73, 0x98, 0xbd, 0xb8, 0xbd, 0x19, 0x0d,
    0xc9, 0xe7, 0x54, 0xd3, 0x39, 0x90, 0x2b, 0xa5, 0x34, 0xf9, 0xf0, 0xfe, 0x0f, 0xf2, 0xea, 0xee,
    0x29, 0x79, 0x53, 0x32, 0x6a, 0x21, 0xee, 0x35, 0xe7, 0x62, 0x63, 0x37, 0xf8, 0x95, 0x2a, 0xb6,
    0xd9, 0xa6, 0x34, 0x7b, 0x3b, 0xd7, 0xaa, 0x92, 0x2c, 0x3c, 0xeb, 0xa7, 0x7d, 0x3e, 0xb8, 0x8c,
    0x32, 0x95, 0x2b, 0x1d, 0x9e, 0xf1, 0x29, 0xa7, 0x3c, 0x8b, 0x38, 0x02, 0xf1, 0x39, 0x2d, 0x44,
    0xbe, 0x09, 0xcd, 0xc6, 0x58, 0x28, 0xfc, 0x4a, 0x9c, 0x1b, 0x2a, 0x8d, 0x6f, 0x40, 0x0b, 0x1e,
    0x31, 0x61, 0xca, 0x9c, 0x6e, 0x42, 0x9e, 0xc3, 0x3a, 0xfa, 0xb1, 0x32, 0x56, 0xf0, 0x8d, 0xdf,
    0xc2, 0x0f, 0x33, 0xfc, 0x00, 0x1d, 0xd1, 0x5c, 0xcc, 0xa5, 0x2f, 0x30, 0xd8, 0xec, 0x1f, 0x15,
    0x42, 0xfa, 0x0b, 0x10, 0xf3, 0x85, 0x0d, 0x07, 0xfd, 0xfe, 0x72, 0x11, 0x15, 0x54, 0xcf, 0x85,
    0x0c, 0xfb, 0x51, 0x49, 0x19, 0x13, 0x72, 0x1e, 0x0e, 0x34, 0x14, 0x51, 0xaa, 0xd6, 0xbe, 0x11,
    0x3f, 0xd5, 0xeb, 0x54, 0x69, 0x06, 0xda, 0xc7, 0x27, 0xbb, 0x20, 0xa3, 0x9a, 0x3d, 0xc0, 0x3e,
    0x80, 0xe1, 0xe5, 0x28, 0x8d, 0x9a, 0x33, 0xe1, 0xa0, 0x5c, 0x13, 0xa3, 0x72, 0xc1, 0xc8, 0xd9,
    0x68, 0x34, 0x1e, 0x4c, 0x26, 0x87, 0xa4, 0xc3, 0x26, 0xa9, 0xcb, 0xa4, 0x29, 0x13, 0x95, 0x09,
    0x07, 0x17, 0xe5, 0x1a, 0xab, 0xaf, 0x1b, 0x75, 0xc3, 0xf1, 0xb8, 0x8f, 0xeb, 0xe6, 0x37, 0x22,
    0xfb, 0xac, 0x81, 0xb0, 0xa0, 0x4c, 0xad, 0xc2, 0x3e, 0x19, 0xe2, 0x26, 0xa9, 0x4f, 0x10, 0x3d,
    0x4f, 0x69, 0xa7, 0x7f, 0xee, 0xde, 0xc1, 0x45, 0x37, 0xb2, 0xb0, 0xb6, 0xbe, 0x23, 0xda, 0x52,
    0xdc, 0x2d, 0x86, 0xdb, 0x56, 0xcb, 0x8b, 0x3e, 0x9d, 0x70, 0xda, 0x52, 0xf4, 0xad, 0x2a, 0x91,
    0xa6, 0x13, 0x16, 0xa9, 0x41, 0x38, 0x08, 0x26, 0x88, 0x6a, 0x57, 0xee, 0x4f, 0x5f, 0x8e, 0xe9,
    0x28, 0x9d, 0x1e, 0x1d, 0x08, 0x2e, 0x6b, 0xd4, 0xb9, 0x90, 0x70, 0x90, 0x2c, 0x18, 0xef, 0x84,
    0x2c, 0x2b, 0xfb, 0x9d, 0xdd, 0x94, 0x90, 0x70, 0x91, 0xc3, 0xf7, 0xdb, 0x7d, 0x27, 0xd2, 0x5c,
    0x65, 0x6f, 0xf7, 0x7a, 0x36, 0xc9, 0xc9, 0xbd, 0xac, 0xc1, 0x93, 0x89, 0xd3, 0xe0, 0xb8, 0xf1,
    0x7c, 0xf0, 0x64, 0x48, 0x3f, 0x2d, 0xde, 0x43, 0xbd, 0xa6, 0x28, 0x4f, 0x8b, 0x34, 0x4b, 0xd9,
    0x04, 0x06, 0x27, 0x62, 0x7d, 0xdc, 0xaf, 0x63, 0x2a, 0x53, 0xc7, 0x35, 0xad, 0xac, 0x55, 0xf2,
    0x41, 0x07, 0x91, 0xf2, 0x90, 0x5f, 0x1c, 0xdc, 0xc7, 0xf9, 0x1e, 0x8f, 0x54, 0x12, 0xee, 0xd1,
    0x4f, 0x27, 0xa7, 0x1d, 0xac, 0x11, 0xb9, 0x12, 0xab, 0x46, 0x9d, 0x54, 0xe5, 0x2c, 0xca, 0x2a,
    0x6d, 0x30, 0x53, 0xa9, 0x84, 0xf3, 0xdb, 0x11, 0xc6, 0x23, 0xe5, 0xeb, 0x5c, 0x56, 0xa3, 0x99,
    0xf1, 0x46, 0x29, 0x19, 0xde, 0xe3, 0x21, 0xc1, 0xd0, 0xb4, 0x28, 0xc3, 0x85, 0x5a, 0x82, 0x7e,
    0x80, 0x75, 0x38, 0xb9, 0x18, 0x41, 0xba, 0x3b, 0x33, 0x96, 0xda, 0xca, 0x6c, 0x8f, 0xfa, 0x3a,
    0x08, 0x86, 0x0e, 0xe0, 0xc7, 0xcd, 0x3b, 0x81, 0x77, 0xec, 0xff, 0x60, 0x82, 0x92, 0x04, 0x75,
    0x01, 0x1f, 0x9b, 0xfc, 0xf6, 0xd0, 0x48, 0x21, 0x5d, 0xcf, 0x8f, 0xfb, 0xd9, 0x16, 0x71, 0x35,
    0x1e, 0xda, 0xc5, 0xf9, 0x8f, 0x41, 0xa6, 0x34, 0x75, 0x5c, 0x9c, 0x6c, 0x27, 0xc2, 0xdf, 0x57,
    0x69, 0x69, 0xb5, 0x49, 0x46, 0xd3, 0x94, 0xf1, 0xe9, 0x2e, 0xee, 0x35, 0xb3, 0x21, 0xee, 0x35,
    0xb3, 0xa9, 0x9e, 0x11, 0xb3, 0x98, 0x89, 0x25, 0xc9, 0x72, 0x6a, 0x4c, 0xe2, 0xd5, 0x17, 0x0f,
    0x07, 0xce, 0x62, 0x38, 0xfb, 0xef, 0x9f, 0x3f, 0xdf, 0x93, 0x6b, 0xa1, 0x8b, 0x15, 0xd5, 0xd0,
    0x4e, 0x99, 0x39, 0x36, 0x04, 0xc7, 0x0c, 0xee, 0xc6, 0xe5, 0xec, 0x4d, 0x99, 0x2b, 0xca, 0x70,
    0x92, 0x15, 0x25, 0xda, 0x93, 0x91, 0x66, 0x3c, 0xf1, 0x7d, 0x44, 0x27, 0xce, 0x14, 0x83, 0xd9,
    0x7e, 0x1d, 0xa4, 0x42, 0xc6, 0x3d, 0xf7, 0xa8, 0x4b, 0xac, 0x22, 0x55, 0x93, 0x8c, 0xd4, 0x20,
    0xc9, 0xd7, 0xc2, 0xbf, 0x16, 0x41, 0xdc, 0x2b, 0x67, 0x31, 0x57, 0xba, 0x20, 0x38, 0x32, 0x17,
    0x8a, 0x25, 0xde, 0xcd, 0xab, 0xdb, 0x3b, 0x8f, 0xd0, 0xac, 0x26, 0x9c, 0x78, 0xbd, 0xca, 0x8d,
    0x39, 0x8f, 0x80, 0xcc, 0xdc, 0xb5, 0xf0, 0x8a, 0x2a, 0xb7, 0xa2, 0xa4, 0xda, 0xf6, 0xea, 0x30,
    0x1f, 0x77, 0xa9, 0x47, 0x04, 0x06, 0x56, 0x0e, 0xdb, 0x0f, 0xf5, 0x53, 0x64, 0xe3, 0x6e, 0x12,
    0x69, 0x42, 0xea, 0xab, 0xe4, 0xb5, 0xe3, 0x78, 0x9f, 0x8f, 0x66, 0x19, 0x94, 0x38, 0x8b, 0x6b,
    0x8c, 0x1e, 0xd1, 0xf0, 0xae, 0x12, 0x1a, 0x6a, 0x75, 0x9c, 0x3f, 0xda, 0x40, 0x53, 0xa5, 0x85,
    0xb0, 0xde, 0xec, 0xd6, 0x62, 0xbd, 0x7b, 0x61, 0x0e, 0xa2, 0x34, 0x87, 0x51, 0xd9, 0xba, 0x68,
    0xa3, 0x69, 0x8d, 0xa4, 0xb1, 0x10, 0x82, 0xe8, 0xe1, 0x93, 0x59, 0x4c, 0xc9, 0x42, 0x03, 0x47,
    0x2e, 0xde, 0x5e, 0xf1, 0x43, 0xc3, 0xbc, 0xd9, 0x87, 0x5f, 0x7e, 0x27, 0xcf, 0x70, 0x59, 0xeb,
    0xf3, 0x1c, 0x3b, 0xab, 0x55, 0x9e, 0xa3, 0x3a, 0x4f, 0x6f, 0x5e, 0xc6, 0x3d, 0xba, 0xcf, 0x60,
    0x32, 0x2d, 0x4a, 0x3b, 0x63, 0x2a, 0xab, 0x0a, 0x9c, 0x43, 0xc1, 0x1c, 0xec, 0x8b, 0x1c, 0xea,
    0x9f, 0xcf, 0x36, 0x2f, 0x59, 0xe7, 0x01, 0xf5, 0x6e, 0xa0, 0x64, 0x83, 0x3b, 0xe1, 0x95, 0x74,
    0x32, 0x76, 0xa0, 0xbb, 0x85, 0xa0, 0xd4, 0xb0, 0xc4, 0x88, 0x2b, 0xe0, 0x14, 0x25, 0xec, 0x74,
    0xa3, 0x25, 0xd5, 0x84, 0xb3, 0x44, 0xc2, 0x8a, 0x5c, 0x63, 0xe4, 0x15, 0x2a, 0xd9, 0xb1, 0x0b,
    0x61, 0xba, 0xe7, 0xc6, 0x26, 0x9f, 0xac, 0xd5, 0x92, 0xeb, 0x46, 0xc6, 0x06, 0xce, 0x58, 0x81,
    0xb3, 0x5b, 0xe2, 0x9d, 0xf1, 0x34, 0xe5, 0xc3, 0xb1, 0x57, 0x6f, 0x08, 0x29, 0x41, 0xdf, 0xa1,
    0x7b, 0x13, 0xef, 0xc3, 0x6f, 0xff, 0x92, 0xc6, 0x36, 0x78, 0xd7, 0x0f, 0x56, 0x09, 0x82, 0x80,
    0xdc, 0xe4, 0x40, 0x0d, 0x10, 0xa6, 0x88, 0x54, 0x96, 0x94, 0x6a, 0x85, 0xbc, 0x15, 0xe7, 0x81,
    0xe7, 0x90, 0xad, 0x1d, 0xb0, 0x6f, 0xbe, 0xfa, 0xf2, 0x0b, 0x6b, 0xcb, 0xd7, 0xd8, 0x1f, 0x30,
    0x35, 0xe8, 0x75, 0xa0, 0x4a, 0x90, 0x9d, 0xc6, 0x26, 0xe7, 0x07, 0x7f, 0x9c, 0x5b, 0x5d, 0x81,
    0xdb, 0x95, 0x75, 0xa9, 0x7b, 0xea, 0xdd, 0xad, 0xe0, 0x9d, 0x75, 0xd0, 0xa0, 0x4e, 0x92, 0x64,
    0xd8, 0xef, 0x77, 0xb7, 0x27, 0xd0, 0x07, 0xfd, 0xf4, 0x72, 0x3a, 0x38, 0x81, 0xfe, 0xd7, 0xcf,
    0xfb, 0x5e, 0x93, 0xdb, 0x0a, 0x0d, 0x63, 0x0c, 0xaf, 0xf2, 0x47, 0xe4, 0x35, 0xa4, 0x4a, 0xd9,
    0x9a, 0x8e, 0xf3, 0x3f, 0x72, 0xc1, 0x48, 0xb0, 0x77, 0xa2, 0x00, 0x55, 0xd9, 0xce, 0x51, 0xed,
    0x95, 0x90, 0xf8, 0x37, 0x26, 0xc0, 0xcb, 0xee, 0x2e, 0x70, 0xb0, 0x37, 0x42, 0xb4, 0x3b, 0x9f,
    0xf4, 0x11, 0x49, 0xb4, 0x83, 0xdc, 0xc0, 0x29, 0x1e, 0xe0, 0x63, 0x7c, 0x9d, 0xe0, 0xf9, 0xfb,
    0xd7, 0x03, 0x9e, 0x6b, 0xea, 0x2e, 0x60, 0xc7, 0x7b, 0xbc, 0x27, 0xf7, 0xd8, 0xeb, 0x86, 0xa4,
    0x5e, 0x6a, 0x30, 0x25, 0x7a, 0x00, 0xea, 0xa0, 0x68, 0xb7, 0x43, 0x51, 0x0c, 0x48, 0xd6, 0xe1,
    0x0c, 0xcb, 0x45, 0x38, 0x0d, 0x1a, 0x33, 0xa1, 0x7b, 0xdd, 0x20, 0xe8, 0xb9, 0xff, 0x5b, 0xfe,
    0x07, 0xf5, 0x7b, 0x42, 0xdf, 0xcd, 0x08, 0x00, 0x00
  };

  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html", (const char*)UPDATE_PAGE_GZ, UPDATE_PAGE_GZ_LEN);
}

static const char* webOtaError = nullptr;
static bool webOtaDone = false;
static bool firstChunkVerified = false;

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    webOtaError = nullptr;
    webOtaDone = false;
    firstChunkVerified = false;

    if (!isSameOriginRequest()) {
      webOtaError = "Cross-origin request forbidden";
      Update.abort();
      return;
    }

    if (isFirmwareUpdating() || arduinoOtaActive) {
      webOtaError = "Firmware update already in progress";
      // Do not abort shared Update singleton if another updater (ArduinoOTA) is running
      return;
    }

    {
      DoorStateLock lock;
      if (!lock.acquired) {
        webOtaError = "State lock acquisition timeout";
        Update.abort();
        return;
      }
      if (currentState == STATE_OPENING || currentState == STATE_CLOSING) {
        webOtaError = "Door in motion. Update aborted.";
        Update.abort();
        return;
      }
    }

    if (ESP.getMaxAllocHeap() < 20000) {
      webOtaError = "Low Memory: Reboot before update.";
      Update.abort();
      return;
    }

    Serial.printf("\n[WebOTA] Update file receiving: %s\n", upload.filename.c_str());
    if (!webOtaSuspended) {
      suspendSafetyTask();
      webOtaSuspended = true;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      webOtaError = "Update.begin failed";
      resumeSafetyTask();
      webOtaSuspended = false;
      return;
    }
    if (server.hasHeader("x-MD5")) {
      String expectedMD5 = server.header("x-MD5");
      expectedMD5.trim();
      if (expectedMD5.length() == 32) {
        bool isHex = true;
        for (size_t i = 0; i < 32; i++) {
          if (!isxdigit(expectedMD5[i])) {
            isHex = false;
            break;
          }
        }
        if (isHex) {
          Update.setMD5(expectedMD5.c_str());
          Serial.printf("[WebOTA] Enforcing MD5 checksum verification: %s\n", expectedMD5.c_str());
        }
      }
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (webOtaError != nullptr) return;
    if (!firstChunkVerified) {
      if (upload.currentSize == 0) return;
      if (upload.buf[0] != 0xE9) {
        Serial.println("\n[WebOTA] Rejected: Invalid firmware binary (missing ESP32 magic byte 0xE9)!");
        webOtaError = "Invalid firmware: missing ESP32 magic byte (0xE9)";
        Update.abort();
        if (webOtaSuspended) {
          resumeSafetyTask();
          webOtaSuspended = false;
        }
        return;
      }
      firstChunkVerified = true;
    }
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
      webOtaError = "Flash write failed";
      Update.abort();
      if (webOtaSuspended) {
        resumeSafetyTask();
        webOtaSuspended = false;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (webOtaError != nullptr) return;
    if (!firstChunkVerified) {
      webOtaError = "Empty or invalid firmware stream";
      Update.abort();
      if (webOtaSuspended) {
        resumeSafetyTask();
        webOtaSuspended = false;
      }
      return;
    }
    if (Update.end(true)) {
      webOtaDone = true;
      Serial.printf("[WebOTA] Update successfully completed! Total bytes: %u\n", upload.totalSize);
    } else {
      Update.printError(Serial);
      webOtaError = "Update.end verification failed";
      if (webOtaSuspended) {
        resumeSafetyTask();
        webOtaSuspended = false;
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    webOtaError = "Upload aborted by client";
    webOtaDone = false;
    Update.abort();
    if (webOtaSuspended) {
      resumeSafetyTask();
      webOtaSuspended = false;
    }
    Serial.println("\n[WebOTA] Update aborted by client!");
  }
}

#if ENABLE_ARDUINO_OTA
void setupOTA() {
  ArduinoOTA.setHostname(DEVICE_HOSTNAME);
  ArduinoOTA.onStart([]() {
    arduinoOtaActive = true;
    suspendSafetyTask();
    Serial.println("\n[ArduinoOTA] Wireless firmware update starting...");
  });
  ArduinoOTA.onEnd([]() {
    arduinoOtaActive = false;
    Serial.println("\n[ArduinoOTA] Wireless firmware update complete!");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total > 0) {
      Serial.printf("[ArduinoOTA] Progress: %u%%\r", (progress * 100) / total);
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    if (arduinoOtaActive) {
      Update.abort();
      arduinoOtaActive = false;
      resumeSafetyTask();
    }
    Serial.printf("[ArduinoOTA] Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("ArduinoOTA Wireless Flashing Service started successfully.");
}
#endif

void initWebPortal() {
  const char* headerkeys[] = {"Origin", "Referer", "x-MD5"};
  server.collectHeaders(headerkeys, 3);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/", HTTP_OPTIONS, handleOptions);

  server.on("/state", HTTP_GET, handleGetState);
  server.on("/state", HTTP_OPTIONS, handleOptions);

  server.on("/toggle", HTTP_POST, handleToggle);
  server.on("/on", HTTP_POST, handleOn);
  server.on("/off", HTTP_POST, handleOff);

  server.on("/update", HTTP_GET, handleUpdateForm);
  server.on("/update", HTTP_POST, []() {
    if (!isSameOriginRequest()) {
      if (webOtaSuspended) {
        resumeSafetyTask();
        webOtaSuspended = false;
      }
      webOtaError = nullptr;
      webOtaDone = false;
      server.send(403, "application/json", "{\"status\":\"error\",\"message\":\"Cross-origin request forbidden\"}");
      return;
    }
    bool hasError = !webOtaDone || Update.hasError() || (webOtaError != nullptr);
    if (hasError) {
      if (webOtaSuspended) {
        resumeSafetyTask();
        webOtaSuspended = false;
      }
      String errMsg = webOtaError ? String(webOtaError) : (!webOtaDone ? "No firmware data received or upload aborted" : "Firmware update failed!");
      webOtaError = nullptr;
      webOtaDone = false;
      server.send(500, "application/json", String("{\"status\":\"error\",\"message\":\"") + errMsg + "\"}");
    } else {
      webOtaError = nullptr;
      webOtaDone = false;
      server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Firmware updated successfully! Rebooting ESP32...\"}");
      delay(1000);
      ESP.restart();
    }
  }, handleUpdateUpload);

  server.on("/setup", HTTP_GET, handleSetupForm);
  server.on("/setup", HTTP_POST, handleSetupSave);

  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/calibrate/reset", HTTP_POST, handleCalibrateReset);

  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP Web Server started successfully.");

#if ENABLE_ARDUINO_OTA
  setupOTA();
#endif
}

void handleWebClients() {
  server.handleClient();
}

void handleOTA() {
#if ENABLE_ARDUINO_OTA
  // Never service ArduinoOTA while WebOTA is flashing, or if another update is running and ArduinoOTA isn't active
  if (webOtaSuspended || (!arduinoOtaActive && isFirmwareUpdating())) {
    return;
  }
  if (arduinoOtaActive || (currentState != STATE_OPENING && currentState != STATE_CLOSING)) {
    ArduinoOTA.handle();
  }
#endif
}


