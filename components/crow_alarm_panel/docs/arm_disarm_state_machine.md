# Crow Alarm Panel ARM/STAY/DISARM State Machine

## Overview

ARM, STAY, and DISARM sequences use a simpler model than OUTPUT-select: they are **fire-and-forget code sequences** where each digit waits for a Command response before the next digit is sent. Unlike OUTPUT, there are no ACK barriers or state confirmation requirements per digit—only a final state transition (ARMING → ARMED_AWAY, or ARMED_AWAY → DISARMED).

This document derives the state machine from observed traces (IP, AAP, Control4, and ESPHome keypad behavior).

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

Enum: `ArmDisarmState` (`crow_alarm_panel.h`)

```
IDLE
├─ on arm_away() / arm_stay() with no code
│  └─> ARM_AWAY_PENDING / ARM_STAY_PENDING (single keypress sent)
├─ on arm_away(code) / arm_stay(code) with non-empty code
│  └─> CODE_DIGIT_PENDING (first digit sent immediately)
├─ on disarm(code)
│  └─> CODE_DIGIT_PENDING (first digit sent immediately)

ARM_AWAY_PENDING / ARM_STAY_PENDING (KEY_ARM or KEY_STAY sent, waiting for Command)
├─ on Command(0x14) addressed to us
│  └─> IDLE (sequence done; ARMED_STATE 0x11 follows independently)
├─ on timeout (>1s)
│  └─> IDLE (abort)

CODE_DIGIT_PENDING (a code digit sent, waiting for Command)
├─ on Command(0x14) addressed to us, more digits remain
│  └─> CODE_DIGIT_PENDING (next digit sent immediately; no intermediate READY state)
├─ on Command(0x14) addressed to us, no more digits
│  └─> CODE_ENTER_PENDING (terminal key sent: KEY_ARM, KEY_STAY, or KEY_ENTER)
├─ on timeout (>1s)
│  └─> IDLE (abort)

CODE_ENTER_PENDING (terminal key sent, waiting for final Command)
├─ on Command(0x14) addressed to us
│  └─> IDLE (sequence done; ARMED_STATE 0x11 follows independently)
├─ on timeout (>1s)
│  └─> IDLE (abort)

[Note: ARMED_STATE (0x11) messages are dispatched by the message handler independently;
the state machine does not gate on them.]
```

### Two paths per operation

| Operation | No-code path | Code path |
|---|---|---|
| `arm_away()` | KEY_ARM → ARM_AWAY_PENDING | CODE_DIGIT_PENDING → … → CODE_ENTER_PENDING (terminal: KEY_ARM) |
| `arm_stay()` | KEY_STAY → ARM_STAY_PENDING | CODE_DIGIT_PENDING → … → CODE_ENTER_PENDING (terminal: KEY_STAY) |
| `disarm(code)` | not supported — code always required | CODE_DIGIT_PENDING → … → CODE_ENTER_PENDING (terminal: KEY_ENTER) |

### Code validation

`start_code_sequence_()` filters to numeric characters only and aborts if the result is
empty. **No length validation is performed in the C++ code** — the controller enforces
digit count (typically 4) and times out (~10 s) if the wrong number of digits is entered.

## Keypad Type Variations

Based on trace analysis (logs-3 through logs-5 and logs-25), the ARM/DISARM state machine supports two implementations per keypad type:

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

### Control4 Keypad (Address 0x06) — Direct ARM + code DISARM
- **Direct ARM:** Single KEY_ARM (0x0D) press → Arming
- **Arming delay:** ~28.2s before ARMED_AWAY
- **Countdown marker:** `Command [14.06.AA.00.00.01.80]` appears during the arming delay
- **Code-based DISARM:** Enter code → ENTER → Disarming (captured sample redacted)

### Implementation (ESPHome keypad, address 0x05)
Current C++ implementation in `CrowAlarmPanel`:
1. **arm_away() / arm_stay()** → Direct single keypress (no code), ~100ms
2. **arm_away(code) / arm_stay(code)** → Code sequence + terminal key (KEY_ARM or KEY_STAY), ~600ms
3. **disarm(code)** → Code sequence + KEY_ENTER, ~600ms (code is always required; no toggle path)

Captured from `esphome-aap-keypad-monitor-logs-25.txt`:
- **Arm:** `07:23:52.514` KEY_ARM (ESPHome keypad) → `07:23:52.616` Arming `[11.00.01.00.00]` → `07:24:17.229` `Command [14.05.AA.00.08.01.80]` → `07:24:21.083` Armed Away `[11.01.00.00.00]`
- **Disarm with code:** `07:24:29.281..07:24:29.789` keys `<redacted>, ENTER` → `07:24:29.890` Disarmed `[11.00.00.00.00]`

Direct ARM/DISARM (single keypress) provides fastest user experience.
Code-entry path is available when the controller requires code authentication.

## Key Rules

1. **Single command per keypress:** Each digit waits for Command(0x14) before allowing the next
2. **Two paths for arm:** No-code → single keypress (ARM_AWAY_PENDING / ARM_STAY_PENDING); with code → code sequence (CODE_DIGIT_PENDING)
3. **Code required for disarm:** `disarm()` always takes a code; there is no no-code toggle path
4. **Shared code-sequence helper:** `start_code_sequence_(code, terminal_key)` handles arm-with-code and disarm identically; terminal key differs (KEY_ARM, KEY_STAY, or KEY_ENTER)
5. **No ACK barrier:** Unlike OUTPUT, the code sequence does not wait for an ACK (0x1D)
6. **Timeout recovery:** ~1s per state; if no Command received, abort to IDLE

## Timing Observations

| Event | Duration |
|-------|----------|
| Digit → Command | ~100–110ms (consistent) |
| ENTER → Arming/Disarmed state | ~80–90ms |
| State broadcast to other keypads | ~80–90ms |
| Total sequence (4-digit code) | ~600–700ms |
| Arming delay (logs-25, Control4 and ESPHome) | ~28.2–28.5s |

## Mapping to Current Code

State machine implemented in `crow_alarm_panel.cpp`. Key entry points:

- `arm_away(code)` / `arm_stay(code)` — selects direct or code path, sets initial state, sends first keypress
- `arm_disarm_state_machine` driven in `loop()` under `case KEYPAD_COMMAND:` — transitions on each command addressed to our keypad address
- `start_code_sequence_(code, terminal_key)` — shared helper for arm-with-code and disarm; sets `CODE_DIGIT_PENDING` and sends first digit immediately (idx=1 pre-advanced before the first keypress to avoid re-entry during `send_packet()` delays)
- Watchdog in `loop()`: any non-IDLE state that exceeds 1 s without a `KEYPAD_COMMAND` aborts to IDLE

## Test Scenarios

### Scenario 1: Arm Away (no code)
```
User calls: arm_away()
→ state = ARM_AWAY_PENDING, send KEY_ARM (0x0D)
  ↓ (after Command received ~100ms)
→ state = IDLE, wait for ARMED_STATE message to confirm
```

### Scenario 2: Arm Away (with a 4-digit code)
```
User calls: arm_away("<code>")
→ state = CODE_DIGIT_PENDING, send digit 1
  ↓ (after Command received ~100ms)
→ CODE_DIGIT_PENDING: send digit 2
  ↓ (after Command received ~100ms)
→ CODE_DIGIT_PENDING: send digit 3
  ↓ (after Command received ~100ms)
→ CODE_DIGIT_PENDING: send digit 4, then terminal key KEY_ARM
→ state = CODE_ENTER_PENDING
  ↓ (after Command received ~100ms)
→ state = IDLE, wait for ARMED_STATE message to confirm
```

### Scenario 3: Disarm with a 4-digit code
```
User calls: disarm("<code>")
→ state = CODE_DIGIT_PENDING, send digit 1
  ↓ (after Command received ~100ms)
→ CODE_DIGIT_PENDING: send digit 2
  ↓ (after Command received ~100ms)
→ CODE_DIGIT_PENDING: send digit 3
  ↓ (after Command received ~100ms)
→ CODE_DIGIT_PENDING: send digit 4, then terminal key KEY_ENTER
→ state = CODE_ENTER_PENDING
  ↓ (after Command received ~100ms)
→ state = IDLE, wait for DISARMED message to confirm
Total: ~600ms
```

### Scenario 4: Bus error (no Command after 1s)
```
User calls: arm_away()
→ state = ARM_AWAY_PENDING, send KEY_ARM
  ↓ (timeout after 1s, no Command)
→ state = IDLE, abort, log error
```

## Notes

- ARM/STAY/DISARM sequences are simpler than OUTPUT because there's no ACK handshake
- No-code arm_away()/arm_stay() complete after a single KEYPAD_COMMAND (~100ms total)
- Code-based sequences share the same `CODE_DIGIT_PENDING` / `CODE_ENTER_PENDING` states regardless of whether the operation is arm or disarm
- ARMED_STATE messages (0x11) and state confirmations happen independently; state machine doesn't wait for them
- All three keypads receive Command broadcasts during arming/disarming; only the originating keypad controls the sequence
- `disarm()` also guards against calling when already disarmed — it returns early if `is_armed()` is false
