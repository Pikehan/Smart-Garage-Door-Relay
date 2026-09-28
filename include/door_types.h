#pragma once

enum DoorState {
  STATE_UNKNOWN = 0,
  STATE_CLOSED = 1,
  STATE_OPENING = 2,
  STATE_CLOSING = 3,
  STATE_OPEN = 4,
  STATE_STOPPED = 5
};

// Helper to convert DoorState to human-readable string
inline const char* getDebugString(DoorState s) {
  switch (s) {
    case STATE_CLOSED:  return "Closed";
    case STATE_OPENING: return "Opening";
    case STATE_CLOSING: return "Closing";
    case STATE_OPEN:    return "Open";
    case STATE_STOPPED: return "Stopped";
    default:            return "Unknown";
  }
}
