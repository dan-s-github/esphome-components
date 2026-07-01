# Crow Alarm Panel ARM/STAY/DISARM State Machine

## Overview

ARM, STAY, and DISARM sequences use a simpler model than OUTPUT-select: they are **fire-and-forget code sequences** where each digit waits for a Command response before the next digit is sent. Unlike OUTPUT, there are no ACK barriers or state confirmation requirements per digit—only a final state transition (ARMING → ARMED_AWAY, or ARMED_AWAY → DISARMED).

This document derives the state machine from observed traces (real IP Keypad behavior).

## Sequences from Traces

### ARM Sequence (Code sample: 1234)
```
 0ms  Key 1
102ms  Command (0x14)
115ms  Key 2
204ms  Command (0x14)
306ms  Key 3
335ms  Command (0x14)
409ms  Key 4
512ms  Command (0x14)
551ms  Key ENTER
634ms  Arming (0x11 ARMED_STATE message)
716ms  Commands sent to other keypads (state broadcast)
921ms  Keypad State (0x15) normal
```

**Pattern:** Each digit → Command. ENTER → Arming message.

**Timings:**
- Digit-to-digit spacing: ~100–150ms (Command arrives, then next digit sent)
- Enter-to-Arming state: ~80ms
- Final state broadcast: ~80–90ms

### DISARM Sequence (Code sample: 1234 from armed state)
```
 0ms  Key 1
103ms  Command (0x14)
128ms  Key 2
222ms  Command (0x14)
307ms  Key 3
345ms  Command (0x14)
420ms  Key 4
512ms  Command (0x14)
614ms  Key ENTER
716ms  Disarmed (0x11 ARMED_STATE message)
735ms  Commands sent to other keypads (state broadcast)
921ms  Keypad State (0x15) normal
```

**Same pattern as ARM:** Each digit waits for Command before next. ENTER triggers state transition.

### OUTPUT Sequence (digit "1" to output 8)
```
 0ms  Key OUTPUT
102ms  ACK (0x1D)
133ms  Command (0x14)
217ms  Key 1
307ms  Command (0x14)
333ms  Output state (0x50)
409ms  Key ENTER
440ms  Command (0x14)
512ms  Keypad State (0x15) normal
```

**Differs from ARM/DISARM:** OUTPUT gets explicit ACK before command. Output state broadcast on digit reception.

## States for ARM/STAY/DISARM

```
IDLE
├─ on keypress(ARM) or keypress(STAY) from button/API
│  └─> ARM_PENDING (single keypress, no queue)
├─ on disarm(code) from API
│  └─> DISARM_DIGIT_PENDING (queue all code digits + ENTER)

ARM_PENDING (waiting for Command after ARM keypress)
├─ on Command(0x14) received
│  └─> IDLE (state machine done; ARMED_STATE message follows separately)
├─ on timeout (>1s)
│  └─> IDLE (abort)

STAY_PENDING (waiting for Command after STAY keypress)
├─ on Command(0x14) received
│  └─> IDLE
├─ on timeout (>1s)
│  └─> IDLE

DISARM_DIGIT_PENDING (digit sent, waiting for Command)
├─ on Command(0x14) received
│  └─> DISARM_DIGIT_READY (ready for next digit or ENTER)
├─ on timeout (>1s)
│  └─> IDLE (abort sequence)

DISARM_DIGIT_READY (can send next digit or ENTER)
├─ on more digits queued
│  └─> DISARM_DIGIT_PENDING (send next digit)
├─ on ENTER queued
│  └─> DISARM_ENTER_PENDING

DISARM_ENTER_PENDING (ENTER sent, waiting for Command and state update)
├─ on Command(0x14) received
│  └─> IDLE (state machine done; ARMED_STATE message follows)
├─ on timeout (>1s)
│  └─> IDLE

[Note: ARMED_STATE (0x11) messages and ARMED_STATE state updates happen automatically
in the message handler; the state machine doesn't gate them. They confirm completion
but don't control the sequence flow.]
```

## Keypad Type Variations

Based on trace analysis (logs-3 through logs-5), the ARM/DISARM state machine supports two implementations per keypad type:

### IP Keypad (Address 0x07) — Code-based only
- ARM: Enter code → ENTER → Arming (~600ms)
- DISARM: Enter code → ENTER → Disarming (~600ms)
- Code length: 4 digits (strict validation)
- Invalid code: Controller timeout ~10s, then reject

### AAP Keypad (Address 0x00) — Direct ARM + optional code
- **Direct ARM:** Single KEY_ARM (0x0D) press → Arming (~100ms) [Simple, no code]
- **Direct DISARM:** Single KEY_ARM press again → Disarming (~100ms) [Toggle behavior]
- **Code-based DISARM:** Enter code → ENTER → Disarming (~1.5s for 4 digits)
- Code validation: Same as IP, timeout ~10s for invalid length

### Implementation Note
ESPHome keypad (0x05) should prioritize simplicity:
1. **arm_away()** → Send KEY_ARM (direct, ~100ms)
2. **disarm()** → Send KEY_ARM again (toggle, ~100ms)
3. **disarm(code)** → Send code digits + ENTER (validated path, ~600-1500ms)

Direct ARM/DISARM (single keypress) provides fastest and simplest user experience.
Code-entry path available if controller requires authentication but less frequently used.

## Key Rules

1. **Single command per keypress:** Each digit waits for Command(0x14) before allowing the next
2. **Fire-and-forget for ARM/STAY:** Single keypress, no state machine after Command arrives
3. **Code queue for DISARM:** All code digits are queued upfront, then sent one-by-one with Command gating
4. **No ACK barrier:** Unlike OUTPUT, ARM/STAY/DISARM do not wait for ACK
5. **Timeout recovery:** ~1s per state; if no Command received, abort to IDLE

## Timing Observations

| Event | Duration |
|-------|----------|
| Digit → Command | ~100–110ms (consistent) |
| ENTER → Arming/Disarmed state | ~80–90ms |
| State broadcast to other keypads | ~80–90ms |
| Total sequence (4-digit code) | ~600–700ms |

## Mapping to Current Code

### Current Implementation (from crow_alarm_panel.cpp)
```cpp
void CrowAlarmPanel::arm_away() {
  if (this->arm_in_progress_) return;
  this->arm_in_progress_ = true;
  this->arm_started_ms_ = millis();
  this->keypress(KEY_ARM);  // Single keypress, sent immediately
}

void CrowAlarmPanel::disarm(const std::string &code) {
  if (this->disarm_in_progress_) return;
  this->disarm_in_progress_ = true;
  this->disarm_started_ms_ = millis();
  for (char c : code) {
    if (c >= '0' && c <= '9') {
      this->keypress_queue_.push_back(c - '0');  // Queue digits
    }
  }
  this->keypress_queue_.push_back(KEY_ENTER);  // Queue ENTER
}

// In loop()
if (now_ms - this->last_keypress_sent_ms_ >= 500) {  // 500ms inter-keypress
  uint8_t key = this->keypress_queue_.front();
  this->keypress_queue_.erase(this->keypress_queue_.begin());
  this->keypress(key);
  this->last_keypress_sent_ms_ = now_ms;
  
  if (key == KEY_ENTER && this->disarm_in_progress_) {
    this->disarm_in_progress_ = false;
  }
  if ((key == KEY_ARM || key == KEY_STAY) && this->arm_in_progress_) {
    this->arm_in_progress_ = false;  // Clear flag after ARM sent
  }
}
```

### Issues with Current Code

1. **500ms inter-keypress is too slow** — traces show ~100–110ms inter-digit
2. **No Command-gating** — queue sends digits as fast as 500ms allows, ignoring Command responses
3. **Flag cleared immediately** — `arm_in_progress_` cleared after KEY_ARM sent, not after Command received

### Recommended Fix

Use the same Command-gating as OUTPUT-select, but simpler (no ACK barrier):

1. For ARM/STAY: Send single keypress, set flag, wait for Command, then clear flag
2. For DISARM: Queue code digits, send first digit, set DIGIT_PENDING state, on each Command transition to DIGIT_READY and send next, until ENTER sent and final Command received

## Test Scenarios

### Scenario 1: Arm Away
```
User calls: arm_away()
→ state = ARM_PENDING, send KEY_ARM (0x0D)
  ↓ (after Command received ~100ms)
→ state = IDLE, wait for ARMED_STATE message to confirm
```

### Scenario 2: Disarm with code "1234"
```
User calls: disarm("1234")
→ state = DISARM_DIGIT_PENDING, send digit 1
  ↓ (after Command received ~100ms)
→ state = DISARM_DIGIT_READY, send digit 2
  ↓ (after Command received ~100ms)
→ state = DISARM_DIGIT_READY, send digit 3
  ↓ (after Command received ~100ms)
→ state = DISARM_DIGIT_READY, send digit 4
  ↓ (after Command received ~100ms)
→ state = DISARM_DIGIT_READY, send KEY_ENTER
  ↓ (after Command received ~100ms)
→ state = IDLE, wait for DISARMED message to confirm
Total: ~600ms (vs. current ~2.5s with 500ms pacing)
```

### Scenario 3: Bus error (no Command after 1s)
```
User calls: arm_away()
→ state = ARM_PENDING, send KEY_ARM
  ↓ (timeout after 1s, no Command)
→ state = IDLE, abort, log error
```

## Notes

- ARM/STAY/DISARM sequences are simpler than OUTPUT because there's no ACK handshake
- The main optimization is reducing inter-digit spacing from 500ms (current) to ~100–120ms (observed)
- ARMED_STATE messages (0x11) and state confirmations happen independently; state machine doesn't wait for them
- All three keypads receive Command broadcasts during arming/disarming; only the originating keypad controls the sequence
