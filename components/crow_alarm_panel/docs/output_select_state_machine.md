# Crow Alarm Panel Output-Select State Machine

## Overview

Output-select sequences (OUTPUT → digit(s) → ENTER) require strict synchronization with the controller's ACK/Command responses. The current implementation queues keypresses without waiting, causing the controller to echo ACKs and the panel to become confused.

This document defines the correct state machine derived from observed traces (IP Keypad vs. ESPHome Keypad).

## States

Enum: `OutputSelectState` (`crow_alarm_panel.h`)

```
IDLE
├─ on set_output(n) called
│  └─> OUTPUT_PENDING (digit(s) pre-stored; KEY_OUTPUT sent)

OUTPUT_PENDING (waiting for ACK after OUTPUT sent)
├─ on ACK(0x1D) addressed to us
│  └─> AWAIT_COMMAND
├─ on timeout (>1s)
│  └─> IDLE (abort, error)

AWAIT_COMMAND (ACK received, waiting for first Command response)
├─ on Command(0x14) addressed to us → send next pre-stored digit
│  └─> DIGIT_PENDING
├─ on timeout (>1s)
│  └─> IDLE (abort, error)

DIGIT_PENDING (digit sent, waiting for Command response)
├─ on Command(0x14) addressed to us, more digits remain → send next digit
│  └─> DIGIT_PENDING (stays in same state; no intermediate READY state)
├─ on Command(0x14) addressed to us, no more digits → send KEY_ENTER
│  └─> ENTER_PENDING
├─ on timeout (>1s)
│  └─> IDLE (abort, error)

ENTER_PENDING (ENTER sent, waiting for final Command)
├─ on Command(0x14) addressed to us
│  └─> IDLE (sequence complete)
├─ on timeout (>1s)
│  └─> IDLE
```

Note: digits are pre-computed and stored in `output_select_keys_` before `KEY_OUTPUT` is sent.
They are emitted **one per `KEYPAD_COMMAND`** received — not all at once and not before the ACK.

## Key Rules

1. **OUTPUT is a barrier:** No digit/ENTER can be sent until OUTPUT→ACK→Command sequence completes
2. **Each keypress is acknowledged:** Each digit and ENTER waits for a Command(0x14) response before allowing the next
3. **Timeout recovery:** Each state has a 1–2s abort timeout; exceed it and abort to IDLE
4. **Single active sequence:** Only one OUTPUT sequence can be active; incoming keypresses are dropped if a sequence is already in progress
5. **Non-OUTPUT keypresses are unrestricted:** Normal keypresses (ARM, STAY, ENTER without OUTPUT context, etc.) bypass the state machine

## Mapping to Current Code

State machine implemented in `crow_alarm_panel.cpp`. Key entry points:

- `set_output(n, state)` — guards against concurrent sequences, pre-stores digits in `output_select_keys_`, sets `OUTPUT_PENDING`, then calls `keypress(KEY_OUTPUT)`. State is set **before** the keypress call because `send_packet()` uses `delay()`/`yield()` internally which can re-enter `loop()`. If the `0x1D` ACK arrives during that yield, `loop()` must see `OUTPUT_PENDING` to transition correctly.
- Transitions driven in `loop()` under `case OUTPUT_SELECT_ACK:` (→ AWAIT_COMMAND) and `case KEYPAD_COMMAND:` (AWAIT_COMMAND/DIGIT_PENDING → send next key; ENTER_PENDING → IDLE).
- Watchdog: any non-IDLE state exceeding 1 s without a response aborts to IDLE and clears the key queue.

## Test Scenarios

### Scenario 1: Single-digit output (output 4)
```
User calls: set_output(4, true)
→ output_select_keys_ = [4]; state = OUTPUT_PENDING; send KEY_OUTPUT
  ↓ (ACK 0x1D received)
→ state = AWAIT_COMMAND
  ↓ (Command 0x14 received)
→ send digit 4; state = DIGIT_PENDING
  ↓ (Command 0x14 received, no more digits)
→ send KEY_ENTER; state = ENTER_PENDING
  ↓ (Command 0x14 received)
→ state = IDLE, output activated
```

### Scenario 2: Two-digit output (output 14)
```
User calls: set_output(14, true)
→ output_select_keys_ = [1, 4]; state = OUTPUT_PENDING; send KEY_OUTPUT
  ↓ (ACK received)
→ state = AWAIT_COMMAND
  ↓ (Command received)
→ send digit 1; state = DIGIT_PENDING
  ↓ (Command received, digit 4 still queued)
→ send digit 4; state = DIGIT_PENDING
  ↓ (Command received, no more digits)
→ send KEY_ENTER; state = ENTER_PENDING
  ↓ (Command received)
→ state = IDLE, output activated
```

### Scenario 3: Bus error (no ACK after 1s)
```
User calls: set_output(4, true)
→ state = OUTPUT_PENDING, send KEY_OUTPUT
  ↓ (timeout after 1s, no ACK)
→ state = IDLE, abort, log error
```

## Notes

- This state machine applies **only to OUTPUT sequences** (initiated via `set_output()`)
- Other keypresses (ARM, STAY, code digits, etc.) use the separate `ArmDisarmState` machine — see `arm_disarm_state_machine.md`
- ARM/STAY/DISARM sequences also use Command-gating (not timer-based pacing); see `arm_disarm_state_machine.md`
- The digit pre-storage (`output_select_keys_`) holds the decomposed digits of the output number; for outputs ≥ 10 both the tens digit and units digit are stored
