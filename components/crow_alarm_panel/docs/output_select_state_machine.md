# Crow Alarm Panel Output-Select State Machine

## Overview

Output-select sequences (OUTPUT → digit(s) → ENTER) require strict synchronization with the controller's ACK/Command responses. The current implementation queues keypresses without waiting, causing the controller to echo ACKs and the panel to become confused.

This document defines the correct state machine derived from observed traces (IP Keypad vs. ESPHome Keypad).

## States

Enum: `OutputSelectState` (`crow_alarm_panel.h`). Numbers in `timeout in state N` log lines: `OUTPUT_PENDING`=1, `AWAIT_COMMAND`=2, `DIGIT_PENDING`=3, `ENTER_DELAY`=4, `ENTER_PENDING`=5, `CLEAR_PENDING`=6, `CLEAR_DELAY`=7 (appended last so the older numbers keep their meaning).

```
IDLE
├─ on set_output(n) while another keypress sequence runs (arm/disarm, zone bypass, output)
│  └─> stays IDLE; request queued (one slot, newest wins; a press while one is queued
│      replaces it). Started once all machines are idle — after a further
│      OUTPUT_SELECT_QUEUE_SETTLE_MS (1.5 s) if it waited behind an arm/disarm or zone
│      bypass — and dropped after OUTPUT_SELECT_QUEUE_TIMEOUT_MS (10 s)
├─ on set_output(n), panel in exit delay (last ARMED_STATE = Arming)
│  └─> CLEAR_PENDING (digit(s) pre-stored; KEY_ENTER sent)
├─ on set_output(n), otherwise
│  └─> OUTPUT_PENDING (digit(s) pre-stored; KEY_OUTPUT sent)

CLEAR_PENDING (leading ENTER sent, waiting for its Command)
├─ on Command(0x14) addressed to us, CMD byte not 0x89/0xAA
│  └─> CLEAR_DELAY
├─ (CMD 0x89/0xAA = the controller's own exit-delay screen updates → ignored, keep waiting)

CLEAR_DELAY (OUTPUT_SELECT_CLEAR_DELAY_MS = 100 ms; OUTPUT sent 7 ms after the reply was not ACKed)
└─> send KEY_OUTPUT → OUTPUT_PENDING

OUTPUT_PENDING (waiting for ACK after OUTPUT sent)
├─ on ACK(0x1D) addressed to us
│  └─> AWAIT_COMMAND
├─ on Command(0x14) addressed to us instead of the ACK, selection bitmap (data[3]) empty ("recovering")
│  └─> treated as the ACK + first Command: send first digit → DIGIT_PENDING
├─ on Command(0x14) addressed to us instead of the ACK, data[3] non-zero
│  └─> stay (OUTPUT wasn't taken as output selection; a digit + ENTER would just be typed
│      into normal entry). The watchdog retries the attempt.

AWAIT_COMMAND (ACK received, waiting for first Command response)
├─ on Command(0x14) addressed to us → send next pre-stored digit
│  └─> DIGIT_PENDING

DIGIT_PENDING (digit sent, waiting for Command response)
├─ on Command(0x14) addressed to us, more digits remain → send next digit
│  └─> DIGIT_PENDING
├─ on Command(0x14) addressed to us, no more digits
│  └─> ENTER_DELAY

ENTER_DELAY (OUTPUT_SELECT_ENTER_DELAY_MS = 60 ms, lets the output-fire broadcast clear the bus)
└─> send KEY_ENTER → ENTER_PENDING

ENTER_PENDING (ENTER sent, waiting for final Command)
├─ on Command(0x14) addressed to us
│  └─> IDLE, then verify (below)

Watchdog (any non-IDLE state, >1 s without progress):
├─ CLEAR_PENDING / CLEAR_DELAY / OUTPUT_PENDING / AWAIT_COMMAND / DIGIT_PENDING, first time
│  └─> one automatic retry of the whole attempt (leading ENTER again if still in exit delay)
└─ otherwise (second timeout, or ENTER_DELAY/ENTER_PENDING — the output may already have fired)
   └─> IDLE (abort), then verify
```

**Verification (outputs 1–8).** The controller acks every key even when it ignores the selection, so reaching `ENTER_PENDING` → IDLE proves nothing on its own. The sequence counts as landed if, while it ran, either:
- `OUTPUT_STATE` (0x50) showed the output's bit different from its value when the sequence started, or
- a `KEYPAD_COMMAND` to us after the digit went out carried the output's bit in data[3], **and** an earlier one in the same attempt showed that bit clear. That byte is this keypad's selected-output bitmap. It is reset to 0 when OUTPUT is accepted and set when the digit is accepted. Outside output selection it still holds the last selection, e.g. `14.05.89.00.08…` at arming. On 2026-10-14 a stale `08` throughout a sequence the controller never opened was misread as success, so the bit only counts after it has been seen clear.

If neither happened, a WARN `Output-select: output N did not switch …` is logged and the switch is republished from the last `OUTPUT_STATE`. There is no automatic re-send: if the evidence was lost on the wire but the output did fire, a re-send would pulse it twice (e.g. open and close a garage door).

Note: digits are pre-computed and stored in `output_select_keys_` before the first key is sent.
They are emitted **one per `KEYPAD_COMMAND`** received — not all at once and not before the ACK.

## Key Rules

1. **OUTPUT is a barrier:** No digit/ENTER can be sent until OUTPUT→ACK→Command sequence completes
2. **Each keypress is acknowledged:** Each digit and ENTER waits for a Command(0x14) response before allowing the next
3. **Timeout recovery:** Each state has a 1 s no-progress watchdog; the pre-fire states get one automatic retry
4. **One keypress sequence at a time, requests queued:** All three state machines (output, arm/disarm, zone bypass) consume the same Command frames, so only one runs at a time. A `set_output()` that arrives while another is running is held in a one-slot queue rather than dropped.
5. **Exit delay needs a leading ENTER:** During the exit delay the controller puts every keypad on its arming screen (`KEYPAD_COMMAND` 0x89, later 0xAA). An OUTPUT → digit → ENTER sent from there is acked key by key but never selects the output. A physical keypad leaves the screen with ENTER ("Back"), and so does this machine. Outside the exit delay no ENTER is prefixed: ENTER submits whatever the controller has buffered for this keypad, and leftover code digits from an aborted arm/disarm plus ENTER is the arm/disarm-with-code gesture. See `protocol_investigations.md`, "Outputs during the exit delay (2026-10-14)".
6. **Success is verified, not assumed:** see Verification above.

## Mapping to Current Code

State machine implemented in `crow_alarm_panel.cpp`. Key entry points:

- `set_output(n, state)` — queues the request if any keypress sequence is running, otherwise calls `start_output_select_()`, which pre-stores digits in `output_select_keys_`, records the output's current bit, and calls `begin_output_select_attempt_()`. That sets `CLEAR_PENDING` + sends ENTER during the exit delay (`panel_arming_`, tracked from `ARMED_STATE`), else `OUTPUT_PENDING` + sends OUTPUT. State is set **before** the keypress call because `send_packet()` uses `delay()`/`yield()` internally which can re-enter `loop()`. If the reply arrives during that yield, `loop()` must already see the state that expects it.
- Transitions driven in `loop()` under `case OUTPUT_SELECT_ACK:` (→ AWAIT_COMMAND) and `case KEYPAD_COMMAND:` (CLEAR_PENDING → send OUTPUT; AWAIT_COMMAND/DIGIT_PENDING → send next key; ENTER_PENDING → `finish_output_select_()`). Landing evidence is collected in `case OUTPUT_STATE:` and `case KEYPAD_COMMAND:`.
- Watchdog: retry via `begin_output_select_attempt_()`, or abort via `finish_output_select_()`. The queue is drained right after the watchdog in `loop()`.

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
