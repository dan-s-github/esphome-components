# Crow Alarm Panel Output-Select State Machine

## Overview

Output-select sequences (OUTPUT → digit(s) → ENTER) require strict synchronization with the controller's ACK/Command responses. The current implementation queues keypresses without waiting, causing the controller to echo ACKs and the panel to become confused.

This document defines the correct state machine derived from observed traces (IP Keypad vs. ESPHome Keypad).

## States

```
IDLE
├─ on keypress(OUTPUT) from user/call
│  └─> OUTPUT_PENDING

OUTPUT_PENDING (waiting for ACK after OUTPUT sent)
├─ on ACK(0x1D) received
│  └─> AWAIT_COMMAND
├─ on timeout (>1s)
│  └─> IDLE (abort, error)

AWAIT_COMMAND (ACK received, waiting for Command response)
├─ on Command(0x14) received
│  └─> DIGIT_READY
├─ on timeout (>2s)
│  └─> IDLE (abort, error)

DIGIT_READY (can send next keypress: digit, more digits, or ENTER)
├─ on keypress(digit 0-9)
│  └─> DIGIT_PENDING
├─ on keypress(ENTER)
│  └─> ENTER_PENDING
├─ on timeout (>5s idle)
│  └─> IDLE (user took too long)

DIGIT_PENDING (digit sent, waiting for Command response)
├─ on Command(0x14) received
│  └─> DIGIT_READY (ready for next digit or ENTER)
├─ on timeout (>1s)
│  └─> IDLE (abort, error)

ENTER_PENDING (ENTER sent, sequence completion)
├─ on Command(0x14) received
│  └─> COMPLETION
├─ on Keypad State(0x15) received (optional)
│  └─> COMPLETION
├─ on timeout (>1s)
│  └─> IDLE

COMPLETION
├─ cleanup, release locks, log success
│  └─> IDLE
```

## Key Rules

1. **OUTPUT is a barrier:** No digit/ENTER can be sent until OUTPUT→ACK→Command sequence completes
2. **Each keypress is acknowledged:** Each digit and ENTER waits for a Command(0x14) response before allowing the next
3. **Timeout recovery:** Each state has a 1–2s abort timeout; exceed it and abort to IDLE
4. **Single active sequence:** Only one OUTPUT sequence can be active; incoming keypresses are dropped if a sequence is already in progress
5. **Non-OUTPUT keypresses are unrestricted:** Normal keypresses (ARM, STAY, ENTER without OUTPUT context, etc.) bypass the state machine

## Mapping to Current Code Issues

### Current Broken Behavior
```
keypress_queue_ = [OUTPUT, digit, ENTER]
loop() sends each as fast as is_bus_idle_() allows
→ Controller receives OUTPUT then immediately digit
→ No ACK processing, queue pops too fast
→ Controller confused, echoes duplicate ACKs
→ Command responses delayed or lost
→ Sequence fails with cascading ACKs
```

### Fixed Behavior
```
keypress(OUTPUT) → OUTPUT_PENDING, don't queue digit yet
on ACK(0x1D) → AWAIT_COMMAND
on Command(0x14) → DIGIT_READY, allow digit keypress to queue
on keypress(digit) → DIGIT_PENDING
on Command(0x14) → DIGIT_READY, allow ENTER to queue
on keypress(ENTER) → ENTER_PENDING
on Command(0x14) → COMPLETION → IDLE
```

## Implementation Steps

1. **Add state enum:**
   ```cpp
   enum OutputSelectState {
       IDLE,
       OUTPUT_PENDING,
       AWAIT_COMMAND,
       DIGIT_READY,
       DIGIT_PENDING,
       ENTER_PENDING,
       COMPLETION
   };
   ```

2. **Replace current fields:**
   - Remove `waiting_for_output_select_ack_` (bool)
   - Remove `output_select_takeover_pending_` (bool)
   - Remove `output_select_sent_ms_` (uint32_t)
   - Add `output_select_state_` (OutputSelectState)
   - Add `output_select_state_enter_ms_` (uint32_t) for timeout tracking

3. **In `loop()`, add state machine update:**
   - Check for timeouts: if (now_ms - state_enter_ms > timeout) → abort to IDLE
   - Only allow keypress queue advance if state permits
   - Transition state on message receipt (ACK, Command, KeypadState)

4. **In message handler (loop), add state transitions:**
   - `case OUTPUT_SELECT_ACK:` → if state == OUTPUT_PENDING, transition to AWAIT_COMMAND
   - `case KEYPAD_COMMAND:` → if state == AWAIT_COMMAND, transition to DIGIT_READY; if state == DIGIT_PENDING or ENTER_PENDING, transition to DIGIT_READY or COMPLETION
   - `case KEYPAD_STATE:` → if state == ENTER_PENDING, may signal completion

5. **In `set_output()`, initiate OUTPUT sequence:**
   - Clear digit queue
   - Send OUTPUT keypress
   - state = OUTPUT_PENDING
   - Do NOT queue digit/ENTER yet

6. **In `loop()` keypress queue handler:**
   - Only dequeue and send if state permits
   - Check state before sending each keypress

## Test Scenarios

### Scenario 1: Normal output-select (digit 4)
```
User calls: set_output(4, true)
→ state=OUTPUT_PENDING, send OUTPUT
  ↓ (after ACK received)
→ state=DIGIT_READY, send digit 4
  ↓ (after Command received)
→ state=ENTER_PENDING, send ENTER
  ↓ (after Command received)
→ state=IDLE, output activated
```

### Scenario 2: Multi-digit output (output 14)
```
User calls: set_output(14, true)
→ state=OUTPUT_PENDING, send OUTPUT
  ↓ (after Command received)
→ state=DIGIT_READY, send digit 1
  ↓ (after Command received)
→ state=DIGIT_READY (ready for next), send digit 4
  ↓ (after Command received)
→ state=DIGIT_READY (ready), send ENTER
  ↓ (after Command received)
→ state=IDLE, output activated
```

### Scenario 3: Bus error (no ACK after 1s)
```
User calls: set_output(4, true)
→ state=OUTPUT_PENDING, send OUTPUT
  ↓ (timeout after 1s, no ACK)
→ state=IDLE, abort, log error
```

## Notes

- This state machine applies **only to OUTPUT sequences** (0xA1 with data[1]==KEY_OUTPUT context)
- Other keypresses (ARM, STAY, single-digit tones, etc.) are sent normally without state gating
- The queue used for ARM/STAY/disarm sequences (500ms inter-keypress pacing) remains unchanged
- Output-select sequences use a **separate, strictly synchronous path** (no queueing; state-driven)
