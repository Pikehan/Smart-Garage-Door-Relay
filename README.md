# ESP32 Smart Garage Door Relay

> An offline, low-latency ESP32 smart garage door controller with limit switches, persistent memory, and a local web dashboard and REST API.

---

## How This Garage Door Works

This controller interfaces with a standard cyclic garage door opener and two magnetic limit switches:

- **Single-Button Pulse Actuation**: The opener motor operates via a single wall-button circuit that cycles: **Open → Stop → Close → Stop**. The ESP32 pulses an active-LOW relay for 200ms across these terminals to simulate a physical push-button press.
- **Directional Control**: When commanded to open (`/on`) or close (`/off`) while traveling in the opposite direction, the controller executes a safety sequence: it pulses once immediately to stop the door, waits 500ms, and pulses a second time to reverse direction toward your intended target.
- **Limit Switches**: Two magnetic reed switches detect when the door reaches the physical **Closed** (bottom) or **Open** (overhead) positions. Sensor triggers always take priority—if the door is moved manually or via an external remote, the ESP32 automatically updates its state upon switch contact.
- **Safety Timeout**: If the motor travels for more than 27 seconds without reaching a limit switch, the controller marks the door as `STOPPED` and flags a stall warning.

---

## Door States

The controller tracks six discrete states:

| State | Description | Sensor Reading |
| :--- | :--- | :--- |
| **`CLOSED`** | Door is fully closed resting at the floor. | Closed sensor active, Open sensor inactive. |
| **`OPEN`** | Door is fully raised overhead. | Open sensor active, Closed sensor inactive. |
| **`OPENING`** | Motor is running upward toward the open limit switch. | Both sensors inactive. |
| **`CLOSING`** | Motor is running downward toward the closed limit switch. | Both sensors inactive. |
| **`STOPPED`** | Door was halted midway by a toggle button, safety stall, or 27s travel timeout. | Both sensors inactive (previous direction remembered in NVS). |
| **`UNKNOWN`** | Initial startup state before sensor readings and flash memory are verified. | Checked immediately upon boot. |

---

## Getting Connected

When powering on an unconfigured controller, or if your home Wi-Fi credentials change:

1. Connect your phone or laptop to the temporary setup network:
   - **SSID**: `Garage-Door-Setup`
   - **Password**: `garage1234`
2. Open your web browser and go to:
   ```text
   http://192.168.4.1/setup
   ```
3. Select your home Wi-Fi network from the scan list (or enter it manually) and enter your password.
4. Click **Save & Connect**. The ESP32 stores the settings in persistent memory and connects to your local network.
5. Once connected, access the controller directly at:
   ```text
   http://garage-door.local
   ```
   *(or the static/DHCP IP assigned by your router)*

---

## Usage & Controls

### Web Dashboard
Navigate to `http://garage-door.local` in any browser to access the control interface:
- **Live Status Badge**: Displays current state (`OPEN`, `CLOSED`, `OPENING`, `CLOSING`, `STOPPED`).
- **Quick Action Buttons**: One-tap **Open**, **Close**, and **Toggle**.
- **Diagnostics**: Displays real-time limit switch readings, system uptime, and fault flags.

### REST API Endpoints

| Endpoint | Method | Description |
| :--- | :---: | :--- |
| `/` | `GET` | Interactive web dashboard and status telemetry. |
| `/state` | `GET` | Returns JSON status, limit switch states, calibration, and fault flags. |
| `/toggle` | `POST` | Simulates a wall push-button press (opens if closed, closes if open, stops if moving). |
| `/on` | `POST` | Commands the door to OPEN (ignored if already open or opening). |
| `/off` | `POST` | Commands the door to CLOSE (ignored if already closed or closing). |
| `/calibrate/reset` | `POST` | Resets learned door travel duration calibration back to factory defaults. |
| `/reboot` | `POST` | Gracefully reboots the ESP32 controller. |
| `/setup` | `GET`, `POST` | Wi-Fi provisioning configuration portal. |
| `/update` | `GET`, `POST` | Over-the-air firmware update interface and binary upload handler. |

### Example Payloads

```bash
# Check door status
curl -s http://garage-door.local/state

# Open the garage door
curl -X POST http://garage-door.local/on

# Close the garage door
curl -X POST http://garage-door.local/off

# Trigger push-button toggle
curl -X POST http://garage-door.local/toggle
```

**JSON Response (`GET /state`)**:
```json
{
  "state": "CLOSED",
  "open_sensor": false,
  "close_sensor": true,
  "obstacle_warning": false,
  "sensor_fault": false,
  "sensor_timeout_error": false,
  "failed_to_move": false,
  "mid_track_stall": false,
  "last_commanded_direction": "OPENING",
  "uptime_seconds": 1420,
  "position_pct": 0,
  "open_duration_ms": 17000,
  "close_duration_ms": 17000,
  "is_calibrated": false,
  "switch_unseated": false
}
```

---

## Home Assistant Integration

Add the following to your Home Assistant `configuration.yaml` to integrate the door as a native `cover` entity:

```yaml
cover:
  - platform: template
    covers:
      garage_door:
        device_class: garage
        friendly_name: "Garage Door"
        value_template: >-
          {% if is_state_attr('sensor.garage_state', 'state', 'OPEN') %}
            open
          {% elif is_state_attr('sensor.garage_state', 'state', 'CLOSED') %}
            closed
          {% else %}
            open
          {% endif %}
        open_cover:
          service: rest_command.garage_open
        close_cover:
          service: rest_command.garage_close
        stop_cover:
          service: rest_command.garage_toggle

rest_command:
  garage_open:
    url: "http://garage-door.local/on"
    method: POST
  garage_close:
    url: "http://garage-door.local/off"
    method: POST
  garage_toggle:
    url: "http://garage-door.local/toggle"
    method: POST

sensor:
  - platform: rest
    name: "Garage State"
    resource: "http://garage-door.local/state"
    value_template: "{{ value_json.state }}"
    scan_interval: 2
    json_attributes:
      - open_sensor
      - close_sensor
      - obstacle_warning
      - sensor_fault
      - sensor_timeout_error
      - failed_to_move
      - mid_track_stall
      - position_pct
      - switch_unseated
```

---

## Updating Firmware

Update the controller wirelessly through your browser without physical access to the device:

1. Open your browser and navigate to `http://garage-door.local/update` (or your device's assigned IP).
2. Under **Firmware Update**, click **Choose File** and select your `firmware.bin`.
3. Click **Update Firmware**.
4. The controller uploads, verifies the binary, and reboots automatically within 5 seconds.

> [!NOTE]
> Network settings and door state are preserved in NVS flash across updates. Over-the-air updates are automatically locked out while the door is in transit for physical safety.