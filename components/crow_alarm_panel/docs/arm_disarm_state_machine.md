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
├─ on first Command(0x14) addressed to us in this sequence
│  └─> byte[1] is learned as this sequence's "digit accepted" baseline (see Command byte
│      validation below) and treated as valid; state transitions as below
├─ on Command(0x14) addressed to us with byte[1] == baseline, more digits remain
│  └─> CODE_DIGIT_PENDING (next digit sent immediately; no intermediate READY state)
├─ on Command(0x14) addressed to us with byte[1] == baseline, no more digits remain
│  └─> CODE_ENTER_PENDING (terminal key sent: KEY_ARM, KEY_STAY, or KEY_ENTER)
├─ on Command(0x14) addressed to us with byte[1] != baseline (after baseline established)
│  └─> CODE_DIGIT_PENDING (logged, not treated as an anomaly — see logs-18 below; next digit
│      sent immediately as if byte[1] == baseline)
├─ on timeout (>1s)
│  └─> IDLE (abort)

CODE_ENTER_PENDING (terminal key sent, waiting for final Command)
├─ on Command(0x14) addressed to us
│  └─> CODE_ENTER_PENDING (logged only, proves the controller is still responding; byte[1] is
│      not used to judge success or failure here — see "CODE_ENTER_PENDING success detection
│      redesigned" below for why)
├─ on ARMED_STATE (0x11) matching the request's intent (Disarmed for a disarm; Arming or
│  Armed Away for an arm — dispatched independently by the message handler)
│  └─> IDLE (success; applies from ANY non-IDLE state, including a retry backoff wait — see
│      "Retry backoff + intent-matched ARMED_STATE resolution" (2026-08-22) below)
├─ on timeout (>1s)
│  └─> retry with growing backoff, up to ARM_DISARM_MAX_RETRIES; then IDLE (abort; failure)

[Note: ARMED_STATE (0x11) messages are dispatched by the message handler independently;
entity state is always published from them regardless of the state machine.]
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
6. **Timeout recovery:** ~1s per state; on timeout the sequence is retried up to
   `ARM_DISARM_MAX_RETRIES` times with a growing backoff between attempts (see the 2026-08-05
   and 2026-08-22 entries below), then aborts to IDLE
7. **Command byte validation:** `CODE_DIGIT_PENDING` does not hardcode the "digit accepted" byte[1] value to 0x01. It learns the value from the first response of each sequence for diagnostics, but no longer aborts if a later response doesn't match that baseline — see "Digit-ack byte is keypad-address-specific, not a validity signal" below for why that broke ESPHome-initiated sequences, and "CMD-byte-change abort removed" (2026-07-25, logs-18) below for why the mid-sequence-abort behavior itself was removed. `CODE_ENTER_PENDING` **also does not use byte[1]** to judge success or failure — see "CODE_ENTER_PENDING success detection redesigned" (2026-07-12) below for why that was proven unreliable in both directions. Both states now rely solely on an independent `ARMED_STATE` broadcast for success, or the shared 1s watchdog for failure.

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

## Observed Failure Modes (from logs-6 / logs-32)

Three distinct failure patterns observed when arm/disarm doesn't work:

**1. Timeout in digit state (e.g. state 3):** A digit packet is sent but the controller never responds with a KEYPAD_COMMAND. Cause: transient bus contention or the controller was busy (an anomalous Output State packet `[00.0A.04]` appeared simultaneously in the monitor). Already handled by the 1-second watchdog.

**2. Controller rejects mid-sequence (`0x07` at digit 3):** The controller begins returning `0x07` instead of `0x01` starting part-way through the code sequence. Without command byte validation the state machine would silently advance through the remaining digits and terminal key, log "Code sequence: complete", but the panel would not arm/disarm. Originally fixed by aborting on any `data[1] != 0x01` in `CODE_DIGIT_PENDING`, later loosened to a learned-baseline comparison (2026-07-08 below), and the abort itself removed entirely (2026-07-25, logs-18, near the end of this document) once it was shown to cause more harm than the unverified anomaly it guarded against.

**3. Terminal key lost in bus collision (`0x01` at ENTER):** ESPHome's terminal-key packet collides with the controller's concurrent periodic KEYPAD_COMMAND. The monitor shows a garbled packet `[14.A1.05.11]` (controller's `0x14` type byte wins bus arbitration but ESPHome's `A1.05.11` payload dominates). The controller never receives ENTER. ESPHome receives the collision's survivor KEYPAD_COMMAND (`0x01`) and mistakes it for the ENTER ACK, declaring the sequence done. Originally fixed by aborting in `CODE_ENTER_PENDING` when `data[1] == 0x01`; superseded by the 2026-07-12 redesign below once byte[1] was shown to be unreliable in both directions — `CODE_ENTER_PENDING` no longer inspects it at all, relying solely on the `ARMED_STATE` broadcast or the shared 1s watchdog.

## Digit-ack byte is keypad-address-specific, not a validity signal (2026-07-08)

**Observed facts** (`esphome-aap-alarm-interface-logs-7.txt`, `esphome-aap-keypad-monitor-logs-33.txt`, captured simultaneously from the active interface and a passive monitor): across four separate code-entry attempts (2 disarm, 2 arm-away) by the ESPHome virtual keypad (address `0x05`), the controller's response to the *first* code digit was `Command [14.05.07...]` (byte[1] = `0x07`) every single time — never `0x01`. In the same window, a manual disarm entered on the physical IP keypad (address `0x07`) got byte[1] = `0x01` on every digit and completed successfully with the *same* code (`4286`, confirmed against `secrets.yaml`). The `0x07` responses occurred both while armed (`armed_state` byte = `0x01`, during disarm) and while disarmed (`armed_state` byte = `0x00`, during the subsequent arm attempts), ruling out an "alarm-pending" explanation tied to armed state.

**Inference (medium confidence — 4 consistent observations, single session):** byte[1] of `KEYPAD_COMMAND` in the digit-ack context is a per-keypad-address display code (LED/beep style), not a code-correctness signal. Different keypad types/addresses apparently use different codes for the same "digit accepted, more expected" semantic — address `0x05` uses `0x07` where the IP keypad uses `0x01`. This is consistent with `protocol_wire_format.md`'s `display_code` table describing byte[1] as display/mode instructions, not a validation result, and with the fact that the controller only appears to check code correctness at ENTER (per keypad_protocol_types.md: "Invalid code: Controller timeout ~10s, then reject" — no per-digit rejection documented for the IP keypad).

**Assumption (unverified):** this also explains the original logs-6 observation ("`0x07` at digit 3") — rather than a genuine wrong-code rejection, that may have been the display_code baseline changing mid-sequence for a different reason. The `CODE_DIGIT_PENDING` fix (learn-then-compare baseline instead of hardcoded `0x01`) preserves the ability to abort on a value that changes mid-sequence, so a true anomaly is still caught.

**Not yet verified:** whether letting the sequence continue past digit 1 (baseline learned as `0x07`) results in a successful arm/disarm for address `0x05` — the previous hardcoded check aborted before reaching digit 2 in every captured attempt. Needs a fresh trace with the fix applied.

**Update (2026-07-12): the "stable per-address baseline" theory above does not hold — see below.**

## Digit-ack byte instability contradicts the per-address theory (2026-07-12)

**Source:** `esphome-aap-alarm-interface-logs-10.txt` / `esphome-aap-keypad-monitor-logs-35.txt`, captured simultaneously (active interface + passive monitor), single session, 2026-07-12 09:54–09:57.

**Observed facts:** In one session, the ESPHome keypad (address `0x05`) made two consecutive disarm attempts with the same 4-digit code, ~25 s apart. Both aborted before completing; the user fell back to disarming from the physical IP keypad (`0x07`), and no `ARMED_STATE` (`0x11`) disarm broadcast appears between either abort and the eventual manual disarm — i.e. both aborted attempts genuinely did not disarm the panel, they were not false negatives.

- **Attempt 1** (`09:55:57`): all four digit acks are `byte[1] = 0x01` (baseline learned as `0x01`, not `0x07` — already a contradiction of the 2026-07-08 "address `0x05` always uses `0x07`" observation). Sequence reaches `CODE_ENTER_PENDING`; the ack following the terminal key is *also* `0x01` → aborts as "terminal key lost" (this is failure mode 3, already documented above — not new). The keypad-monitor log for this window shows an extra `Command [14.05.01...]` echo with no corresponding keypress between the digit-8 and digit-6 presses, and shows no ack at all after the ENTER keypress — consistent with a collided/garbled frame near the end of the burst.
- **Attempt 2** (`09:56:22`, same code): digits 1–3 ack `0x01` (matching attempt 1's baseline), but digit 4 (the last digit, immediately before the terminal key would be sent) acks `0x07` → aborts via the `CODE_DIGIT_PENDING` "byte changed from baseline" guard (`crow_alarm_panel.cpp:546`).

**Inference (low confidence — single session, 2 observations):** the digit-ack byte (`data[1]` during code entry) is not a stable per-keypad-address value as previously inferred from logs-7/33 — the same address got `0x01` throughout attempt 1 and for 3 of 4 digits in attempt 2. Both failures in this session cluster at the *last* packet of the code-entry burst (ENTER in attempt 1, the final digit in attempt 2), which is more consistent with an intermittent bus-contention/collision artifact that becomes likelier late in a rapid TX burst than with a meaningful per-address display code.

**Assumption (unverified):** whether the `0x01`→`0x07` flip on the final digit in attempt 2 reflects genuine bus corruption (in the spirit of the `CURRENT_TIME` bit-corruption glitch documented in `protocol_investigations.md`) or an actual controller-side signal is not established from `ESP_LOGD`-level traces alone — there's no raw frame/bit data to distinguish "corrupted 0x01" from "real 0x07". A future capture with `ESP_LOGV` enabled (or a raw CLK/DAT capture, as used for the `CURRENT_TIME` investigation) during a reproduced disarm-abort is needed before changing the `CODE_DIGIT_PENDING` abort guard — tightening or loosening it without that evidence risks either masking a genuine rejection or continuing to abort on benign noise.

**Not yet verified:** the 2026-07-08 "learn baseline `0x07` for address `0x05`, abort on change" fix has still never observed a full successful ESPHome-initiated disarm past digit 1 with a non-`0x01` baseline — both sessions since (logs-7/33 and now logs-10/35) end in an abort. No confirmed successful ESPHome-keypad disarm-with-code trace exists yet.

## Root-cause candidate: hardware-ACK release corrupts our own RX decode under rapid retransmission (2026-07-12)

**Source:** `esphome-aap-alarm-interface-logs-11.txt` / `esphome-aap-keypad-monitor-logs-36.txt`, `ESP_LOGV` enabled (raw frames visible), captured simultaneously from the active interface and a passive monitor, same physical bus, 2026-07-12 10:27–10:36. This is the fresh verbose capture the previous entry asked for.

**Session shape:** one isolated disarm (`10:33:16`) and one arm-with-code (`10:35:01`) complete cleanly — every digit acks `0x01` from first to last, including the final digit, on both. Immediately after the following arm-with-code reaches `Armed Away` (`10:35:30.462`), **four consecutive ESPHome-initiated disarm attempts in the same ~46 s window all fail**: two ack-timeouts (`10:35:33`, `10:35:50`), one `CODE_DIGIT_PENDING` abort on a `0x01`→`0x07` flip at the last digit (`10:36:03`), and one more timeout (`10:36:10`). The panel is eventually disarmed by some other means (no `Code sequence: complete` or `Disarm` log precedes the `Disarmed` broadcast at `10:36:16`).

**Observed facts — decode divergence between the two devices:**

- At `10:35:30.720`–`10:35:31.329` (right as the panel broadcasts `Armed Away`), the passive monitor decodes **eleven identical, clean, well-formed frames** `Command [14.05.04.00.40.01.80]` addressed to the ESPHome keypad (`0x05`) — i.e. the controller retransmitting the same `KEYPAD_COMMAND` roughly every 70–100 ms, consistent with the up-to-10× retry-on-missing-hardware-ACK behavior already documented in `protocol_investigations.md`'s "Hardware ACK" section.
- The **active interface, at the same bus timestamps, decodes the identical traffic as garbage**: one `Unknown [fe.]` followed by nine `Unknown [ff.]` frames (`crow_alarm_panel.cpp:737`), not a single valid `KEYPAD_COMMAND`. Two independent receivers on the same physical bus produced completely different decodes of the same frames — this is a **receiver-side decode fault on the interface itself**, not bus corruption (which would be expected to affect both receivers, as it did for the `CURRENT_TIME` glitch).
- ~3 s later, the interface's first disarm digit (`10:35:33.941`, digit `4`) times out after ~1 s (`crow_alarm_panel.cpp:805`); the ack for it is eventually seen by the interface at `10:35:35.987` — nearly 2 s after send, well past the watchdog.
- The `10:35:50` timeout instead shows genuine silence on **both** logs for ~1.6 s around the missing ack (no frames at all, on either receiver) — a different signature (real controller stall, not a decode fault).
- The `10:36:03` mid-sequence `0x01`→`0x07` flip (previously flagged as unresolved) is, in this capture, a single clean frame `[14.05.07.00.40.01.80]`, byte-identical on both the interface and the monitor, with no adjacent corruption or retransmission — ruling out simple bit-flip noise for *this specific instance*. Its meaning is still not established (see previous section).

**Inference (medium confidence — one session, but a concrete mechanism identified in code):** `CrowAlarmPanelStore::interrupt()` (`crow_alarm_panel.cpp:82-90`) treats the **first clock edge after driving DAT low for the hardware ACK** as a dedicated "release ACK" edge — it returns immediately without sampling a data bit on that edge (by design, for the normal single-ACK case). Under a rapid back-to-back retransmission burst (the controller resending the same frame ~10× in under a second because it isn't seeing our ACK land where it expects), this edge-consuming release happens repeatedly in a tight loop with the normal receive-bit state machine (`inside_`/`num_bits_`/`boundary_buffer_`). A small timing drift between our release edge and the controller's next transmission would misalign our bit count for the rest of that frame — the same class of single-bit-shift corruption already characterized for the `CURRENT_TIME` glitch, but here recurring across many frames in one burst instead of a single field, which is consistent with decode degrading all the way to `0xFF`/`0xFE` garbage rather than a single wrong byte. This would explain why the *passive* monitor (which never drives DAT and has no ACK-release logic) decodes the same burst perfectly.

**Assumption (unverified):** whether this ACK/RX-decode interaction is the actual cause of the *subsequent* disarm timeouts (i.e. whether the controller's internal state for keypad `0x05` remains degraded for several seconds after a failed ACK burst) is circumstantial — timing proximity, not proven causation. The `10:35:50` timeout's clean dual-silence signature argues at least one of the four failures has a different cause (controller-side stall unrelated to our ACK path). No raw CLK/DAT bit capture (as used for the `CURRENT_TIME` investigation) exists yet to confirm the bit-shift mechanism directly at the ISR level.

**Practical takeaway:** unlike the previous entry, this capture points at a specific, addressable code path (`crow_alarm_panel.cpp:82-90`, the ACK-release/RX-decode interaction) rather than pure bus noise — worth a targeted look if a fix is undertaken, but not yet changed here pending confirmation.

### Fix applied (2026-07-12)

`CrowAlarmPanelStore::interrupt()` no longer `return`s immediately after releasing the hardware ACK pin. The ACK-release edge now falls through into the same glitch-filter/data-bit/boundary-detection path every other edge goes through, instead of being discarded outright. Rationale: if the controller's next transmission (e.g. an immediate retry) begins on that same edge, its first bit is now captured instead of silently dropped — the drop was shifting bit alignment for the rest of that frame by one bit, which is consistent with the `Unknown [ff.]`/`[fe.]` garbling observed in logs-11/36 during a ~10x retry burst.

This compiles and passes `esphome config`/`esphome compile` against `crow_alarm_panel_test.yaml`, but **has not been validated against real hardware** — there is no bit-level (CLK/DAT) capture confirming the exact corruption mechanism, only the ISR code inspection plus the ESP_LOGV trace correlation above. Needs a fresh `esphome-aap-alarm-interface`/`esphome-aap-keypad-monitor` capture across several disarm attempts (ideally reproducing the same post-Armed-Away retry burst) to confirm the `Unknown [ff.]`/`[fe.]` decode failures no longer occur and that disarm succeeds reliably on the first attempt.

### First hardware validation of the ISR fix, and a bigger problem found (2026-07-12, logs-12/37)

**Source:** `esphome-aap-alarm-interface-logs-12.txt` / `esphome-aap-keypad-monitor-logs-37.txt`, `ESP_LOGV` enabled, captured after the ISR fix above was flashed. Five arm/disarm attempts in one session: 1 no-code arm, 1 arm-with-code, 3 disarms.

**ISR fix looks effective:** no `Unknown [ff.]`/`[fe.]` decode failures appear anywhere in this capture, including around the arm-with-code → Armed Away transition that previously triggered the retry-burst corruption in logs-11/36. 3 of 5 sequences completed cleanly this time (vs. 0 of 4 disarms in the previous session).

**But 2 of 5 still failed, and one of them exposed a bigger, pre-existing problem:**

- **Disarm attempt 1** (`11:10:19`): fails with the existing `CMD 0x01 in CODE_ENTER_PENDING` ("terminal key lost") abort. No `Disarmed` broadcast ever follows before the next attempt — a genuine failure, correctly identified.
- **Disarm attempt 2** (`11:10:27`): fails with the *same* abort message at `28.130` — but a `Disarmed [11.00.00.00.00]` broadcast arrives 92ms later at `28.232`. **The terminal key was not lost; the disarm succeeded**, and the state machine aborted anyway. This is a confirmed false negative.
- **Disarm attempt at `11:11:39`** completed with **zero ambiguity**: every digit and the terminal key got a clean, consistent `0x07` ack (never `0x01`), so it logged `"Code sequence: complete"` via the pre-existing, unchanged logic. **No `Disarmed` broadcast ever followed** (confirmed independently on both the interface and monitor logs), and the user disarmed again 7 seconds later — implying the panel was still armed. This is a confirmed false positive, and it did not require any ambiguous byte at all: the existing "first non-`0x01` ack after the terminal key means success" assumption is unreliable even in the clean case.

**Inference (medium confidence):** `KEYPAD_COMMAND` byte[1] following the terminal key is not a trustworthy success/failure signal at all, in either direction — not just in the ambiguous-`0x01` case. The `ARMED_STATE` (`0x11`) broadcast is the controller's own authoritative state announcement and is unaffected by whatever the `CODE_ENTER_PENDING` byte[1] heuristic concludes (the message handler already publishes it unconditionally, independent of `arm_disarm_state_`).

### CODE_ENTER_PENDING success detection redesigned (2026-07-12)

Given the above, `CODE_ENTER_PENDING` no longer inspects `KEYPAD_COMMAND` byte[1] to decide success or failure. A `KEYPAD_COMMAND` addressed to us in this state is now only logged (proof the controller is still responding) and does not change state. The sequence resolves only two ways:

- **Success:** an `ARMED_STATE` broadcast recognized as `Arming`, `Armed Away`, or `Disarmed` arrives while `CODE_ENTER_PENDING` — this is checked directly in the `ARMED_STATE` handler (`crow_alarm_panel.cpp`), independent of the `KEYPAD_COMMAND` switch. `Arming` counts as confirmation for an arm-with-code request (it's the fast transitional broadcast; `Armed Away` itself can take ~28s of exit delay per the earlier ARM/DISARM sequence traces).
- **Failure:** the existing shared 1s watchdog (`crow_alarm_panel.cpp` — "abort if any non-IDLE state exceeds 1s without progress") times out with no `ARMED_STATE` confirmation.

**Validated by replay:** re-checking all 5 attempts in logs-12/37 against this new logic (using the independently-observed `ARMED_STATE`/`Disarmed` broadcasts as ground truth) — every one now resolves to the correct outcome: both disarm attempt 1 and the `11:11:39` attempt correctly time out as failures (previously the latter was a false positive), disarm attempt 2 and the `11:11:47` attempt correctly resolve via the `Disarmed` broadcast, and the arm-with-code attempt correctly resolves via the `Arming` broadcast.

Compiles and passes `esphome config`/`esphome compile`. **Not yet validated on real hardware** — needs a fresh capture to confirm no regressions (e.g. that the `Arming`/`Armed Away` broadcast ordering relative to `CODE_ENTER_PENDING` entry holds up across more sessions, and that resolving via `ARMED_STATE` doesn't measurably slow down the reported state change from a user's perspective).

## Watchdog abort stranded the alarm_control_panel entity in ARMING/DISARMING (2026-07-22, logs-16)

**Observed facts** (`esphome-aap-alarm-interface-logs-16.txt`): a disarm-with-code request (`14:42:07`) reached `CODE_ENTER_PENDING`, then the shared 1s watchdog aborted it (`Arm/disarm: timeout in state 4, aborting`, `14:42:08.657`) with no `ARMED_STATE` confirmation. All following user actions were rejected by ESPHome's own `alarm_control_panel` validation: `Cannot disarm when not armed` (`14:42:32`) and `Cannot arm when not disarmed` (`14:42:43`, after the user tried arming instead). The panel was still genuinely armed the whole time — a code entered directly on the physical IP keypad at `14:42:58` disarmed it successfully, confirmed by the `Disarmed [11.00.00.00.00]` broadcast at `14:42:59.233`.

**Root cause:** `CrowAlarmControlPanel::control()` (`crow_alarm_control_panel.cpp`) optimistically calls `publish_state(ACP_STATE_ARMING)` / `publish_state(ACP_STATE_DISARMING)` immediately when `arm_away()`/`arm_stay()`/`disarm()` is called, before the bus sequence resolves. The arm/disarm watchdog (`crow_alarm_panel.cpp`, "abort if any non-IDLE state exceeds 1s without progress") reset only the internal `arm_disarm_state_` back to `IDLE` on timeout — it never touched `alarm_control_panel_`. With no `ARMED_STATE` broadcast to correct it (the failure case, by definition), the entity was left permanently in `ACP_STATE_DISARMING`/`ACP_STATE_ARMING`, which is neither `ACP_STATE_DISARMED` nor an armed/pending/arming state — so ESPHome's `AlarmControlPanelCall::validate_()` rejects *both* future arm and disarm calls from that point on, with no self-recovery short of a genuinely-successful ARMED_STATE broadcast arriving through some other path (here, a physical keypad disarm).

**Fix applied:** added `CrowAlarmPanel::last_confirmed_acp_state_`, updated alongside every non-optimistic `alarm_control_panel_->publish_state()` call — both the `ARMED_STATE` handler's controller-confirmed Arming/Armed Away/Disarmed branches, and the "assume disarmed on zone motion"/"pending on zone alarm" heuristic branches (best-effort inferences, not controller confirmation) — i.e. everywhere except the optimistic `ACP_STATE_ARMING`/`ACP_STATE_DISARMING` publishes in `crow_alarm_control_panel.cpp`, which by design run ahead of confirmation. The arm/disarm watchdog's abort branch now republishes `last_confirmed_acp_state_` to `alarm_control_panel_`, restoring the entity to that last known-good state (controller-confirmed where available, heuristic otherwise) instead of leaving it stuck in the transitional state. Compiles and passes `esphome config`/`esphome compile` against `crow_alarm_panel_test.yaml`.

**Validated on real hardware (2026-07-22, logs-17):** `esphome-aap-alarm-interface-logs-17.txt`, captured with the fix flashed (log line numbers match the post-fix source, e.g. the watchdog abort at `crow_alarm_panel.cpp:832`). A disarm-with-code attempt at `15:37:49` reached the terminal key and got a `0x07` ack, then no `ARMED_STATE` broadcast arrived and the watchdog aborted at `15:37:50.668` — a genuine first-attempt failure, same shape as logs-16. This time a second `disarm()` call at `15:37:55.092`, only ~4.4 s later, logged `Disarm` (`crow_alarm_panel.cpp:945`) rather than being rejected — that log line is only reached when `is_armed()` is true and `arm_disarm_state_ == IDLE`, proving the entity had been restored to `ACP_STATE_ARMED_AWAY` instead of staying stuck in `ACP_STATE_DISARMING`. The retry completed and was confirmed via the `Disarmed` `ARMED_STATE` broadcast at `15:37:55.789`/`Code sequence: complete (confirmed via ARMED_STATE broadcast)` at `15:37:55.803`. Confirms the fix resolves the logs-16 lockout in practice, not just in code review.

## CMD-byte-change abort removed from CODE_DIGIT_PENDING (2026-07-25, logs-18)

**Observed facts** (`esphome-aap-alarm-interface-logs-18.txt`): three arm/disarm failures in one session, all disarm attempts. Two aborted via the shared 1s watchdog in `CODE_ENTER_PENDING` (`13:21:35.690`, `13:22:37.834`) and both recovered correctly via the logs-16/17 fix above — a retried disarm succeeded shortly after each. The third (`13:22:45.854`) instead hit the separate `CODE_DIGIT_PENDING` "CMD byte changed from 0x01 to 0x07" guard, mid-digit-entry — only 3 of 4 code digits had been sent when the ack byte flipped. This abort path predates the logs-16/17 fix and was never updated to match it: it reset `arm_disarm_state_` to `IDLE` but never touched `alarm_control_panel_`, so the entity stayed stuck in the optimistic `ACP_STATE_DISARMING` published by `control()`. The fallout is visible directly in the trace: `[alarm_control_panel:086]: Cannot disarm when not armed` (`13:23:14.419`) rejected a follow-up disarm call, and the user had to disarm from the physical IP keypad (`13:23:29`–`13:23:30`) to force a fresh `ARMED_STATE` broadcast and clear the stuck state.

**Decision:** rather than adding the same restore call to this abort branch (mirroring the watchdog fix), the abort behavior itself was removed instead. Its justification was always weak: the 2026-07-08 and 2026-07-12 entries above already document that this byte's meaning was never established, and that a mid-sequence value change may just be display-code noise rather than a genuine rejection — the same category of unreliable `byte[1]` signal `CODE_ENTER_PENDING` already stopped trusting for the same reason (2026-07-12 above). In this specific logs-18 instance the sequence was aborted before even reaching the terminal key, so whether it would have succeeded was never allowed to be observed. `CODE_DIGIT_PENDING` now behaves like `CODE_ENTER_PENDING`: it logs a mismatch against the learned baseline for diagnostics but continues advancing digits regardless, leaving success/failure entirely to the `ARMED_STATE` broadcast or the shared 1s watchdog.

Compiles and passes `esphome config`/`esphome compile` against `crow_alarm_panel_test.yaml`.

**Validated on real hardware (2026-07-25, logs-21):** `esphome-aap-alarm-interface-logs-21.txt` (`ESP_LOGV` enabled) captured the exact scenario unprompted, mid-session: at `15:02:00.824`, `Arm/disarm: CMD byte changed from 0x01 to 0x07 in CODE_DIGIT_PENDING, continuing` fired after the third code digit, and the sequence carried on to send the terminal key normally (`15:02:00.825`–`.831`) instead of aborting. That attempt went on to fail via the ordinary `CODE_ENTER_PENDING` watchdog (`15:02:01.876`, unrelated to the CMD-byte change — see the genuine-silence entry below), but critically the very next `disarm()` call (`15:02:06.053`) logged `Disarm` immediately rather than being rejected, proving the entity was never stuck. Confirms the fix behaves as intended on real hardware: the CMD-byte change no longer aborts, and no entity lockout occurs regardless of how the attempt ultimately resolves.

## `CODE_ENTER_PENDING` silence confirmed genuine, not an RX decode miss (2026-07-25, logs-21)

**Source:** `esphome-aap-alarm-interface-logs-21.txt`, `ESP_LOGV` enabled (raw frames visible). This is the fresh verbose capture the 2026-07-25 logs-18/19/20 investigation trail (see failure-mode #2 and the `CMD-byte-change abort removed` entry above) asked for, to settle whether the `CODE_ENTER_PENDING` watchdog failures — which had shown a 100%-consistent correlation across logs-16 through logs-20 between "a `KEYPAD_COMMAND` ack for the terminal key is logged" and "the attempt ultimately fails" — reflect genuine controller inaction or a receiver-side decode miss of a real `ARMED_STATE` broadcast.

**Observed facts:** Three `CODE_ENTER_PENDING` watchdog timeouts in this session (`15:02:01.876`, `15:02:07.598`, `15:02:12.914`) all show the identical shape at the raw-frame level: every digit ack, the terminal-key ack, and the following `[ESPHome Keypad] In normal state` broadcast all decode as clean, well-formed frames, and then **zero frames of any kind** — not even a garbled/`Unknown` one — appear for the ~700–900ms leading up to each watchdog firing. A fourth disarm attempt in the same session (`15:01:51.821`) instead timed out mid-digit-entry (`CODE_DIGIT_PENDING`, state 3) with the same clean-then-silent signature — consistent with the pre-existing, already-documented failure mode #1 ("digit sent, controller never responds").

**Inference (medium-high confidence — consistent with 5 independent sessions' worth of the ack-log/failure correlation, now with raw-frame confirmation in one of them):** the `CODE_ENTER_PENDING` failures are not a receiver-side artifact — the bus is genuinely quiet. The controller acks the terminal key (proving it received ENTER) but then does not act on it and does not broadcast `ARMED_STATE`, for reasons not established by this capture (not a bus collision, not a decode corruption — both would leave some trace in the raw frames, and none appears).

**Related but distinct finding — RX decode corruption still recurring:** this same capture also shows a burst of 9 consecutive `Unknown [ff.]` frames (`14:59:18.722`–`19.153`, ~400ms) immediately following an unrelated *successful* disarm's follow-up ack exchange — the same signature the 2026-07-12 ISR fix (see above) was intended to resolve. This is **not** implicated in the `CODE_ENTER_PENDING` failures above (those are clean silence, not corruption) but shows the corruption itself was never fully eliminated — see `protocol_investigations.md` for the cross-session evidence and a dedicated writeup.

**Not yet established:** why the controller sometimes fails to act on a correctly-received ENTER. No raw CLK/DAT bit capture or controller-side diagnostic exists to distinguish an internal controller timing/busy condition from something else. Needs further investigation if this failure rate proves disruptive in practice; no code change is proposed here since `CODE_ENTER_PENDING` already handles this correctly (resolves via `ARMED_STATE`/watchdog, no byte[1] trust) — this entry is about confirming *why* it fails, not changing *how* it's handled.

## Two more corroborating sessions, gap-timing candidate raised then undermined (2026-08-05)

**Source:** `protocol_trace_2026-08-05_disarm_after_arm.md` (full detail) — `esphome-aap-keypad-monitor-logs-41/42/43.txt`, `esphome-aap-alarm-interface-logs-22.txt`.

Two more sessions show the identical `CODE_ENTER_PENDING` silence signature from the 2026-07-25 entry above (clean terminal-key ack, then genuine bus silence until the 1s watchdog aborts), bringing the running total to at least 7 independent sessions. One session (paired monitor+interface capture, logs-43/22) raised a candidate explanation — the single failure out of 8 cycles had the shortest armed→disarm gap (7.4s) of the session, while all 7 successes had gaps ≥9.6s — but a second session (logs-42, monitor-only) contradicts it: 5 of 6 failures there occurred at gaps of 21–44s, far longer than the 7.4s failure elsewhere. Net conclusion: gap length alone doesn't explain the failure rate; something session-level (overall bus conditions, an unidentified controller state) more likely dominates. Still not established. No code change proposed.

## Gap-timing lead conclusively dead; monitor-side RX corruption complicates the 2026-07-12 ACK theory (2026-08-05, logs-44/23)

**Source:** `protocol_trace_2026-08-05_disarm_after_arm.md` (full detail) — `esphome-aap-keypad-monitor-logs-44.txt`, `esphome-aap-alarm-interface-logs-23.txt`.

A third session in the same investigation kills the gap-timing lead outright: cycle 3's disarm failed twice (`CODE_ENTER_PENDING` silence, same signature as always) at 6.4s and 9.5s armed→disarm gaps, then succeeded on a third attempt at 13.8s — but cycle 5 in the *same session* succeeded with a 5.5s gap, shorter than either of cycle 3's failures. Gap length is not the mechanism, in either direction.

Separately, cross-checking cycle 3's second failed attempt against the passive monitor's independently bit-decoded traffic (the monitor still has no working DEBUG output — see the stale-firmware note above, still true as of this session) turned up a burst of `Unknown 0xFF`/`0xFE` RX-corruption frames on the **monitor** at `11:23:47.330–47.741`, in a window the interface decoded with zero corruption. This is the reverse pairing from the 2026-07-12 entry above (there, the ACK-driving interface saw the garbage and the passive monitor was clean) — and since the monitor never drives the hardware ACK, this instance can't be explained by that entry's "our own ACK-release desyncs our own RX" mechanism. Whether this is the same underlying phenomenon or a distinct one producing the same garbled byte values is open; needs `ESP_LOGV` on both devices simultaneously to compare at the raw-frame level. Not yet established. No code change proposed.

## Five consecutive disarm failures; failure mode 3 (bus collision) independently reproduced, and a new variant found (2026-08-05, logs-45/24)

**Source:** `protocol_trace_2026-08-05_disarm_after_arm.md` (full detail) — `esphome-aap-keypad-monitor-logs-45.txt`, `esphome-aap-alarm-interface-logs-24.txt`.

A fourth session in the same investigation, and the worst failure streak seen yet: 5 of 6 disarm attempts fail in a row before the 6th succeeds. This session also kills the "armed via a physical keypad" lead from the entry above — this failure cycle was armed via the ESPHome interface's own ARM key, not a physical keypad, contradicting the pattern every prior failure shared.

Cross-checking each failed attempt against the passive monitor's independent bit-decode (again no working DEBUG output there) turns up a mix of causes rather than one repeated mechanism:

- **Attempt 1** independently reproduces this document's "Terminal key lost in bus collision" failure mode (mode 3, above) almost exactly — the monitor decodes a garbled `[14.a1.05.11]` frame at the terminal-key send, byte-for-byte the same pattern originally described from the interface's own logs alone. This is the first time it's been confirmed via a passive monitor's raw bits in a fresh session.
- **Attempt 4** shows a new variant of the same collision class landing on a mid-sequence digit instead of ENTER: the monitor decodes an abnormally long, malformed `OUTPUT_STATE`-typed frame (`data=000406`) spanning several seconds right where digit "6"'s `KEYPRESS` should have been, with `04`/`06` fragments consistent with two keypresses merging. So this collision mechanism isn't ENTER-specific.
- **Attempts 2 and 5** are the ordinary `CODE_ENTER_PENDING` silence signature from the 2026-07-25 entry (clean `0x07` ack, then genuine silence).
- **Attempt 3** is the ordinary "timeout in digit state" failure mode (mode 1, above) — no ack at all for the first digit.

**Inference (low confidence, one session):** a "bad" session appears to raise the odds of several already-catalogued failure mechanisms together (two collisions, two silences, one digit-timeout, all in the same ~30s span) rather than introducing one new mechanism. Combined with logs-42 (5/6 failed) and the mostly-clean logs-22/23 (1/8, 2/6 failed), sessions seem to vary between "good" and "bad" for reasons still not isolated. Not yet established. No code change proposed.

## Bus-collision mode reproduced a third time (now on the first digit); corruption pairing flips back (2026-08-05, logs-46/25)

**Source:** `protocol_trace_2026-08-05_disarm_after_arm.md` (full detail) — `esphome-aap-keypad-monitor-logs-46.txt`, `esphome-aap-alarm-interface-logs-25.txt`.

A fifth session, "medium" severity (3 of 9 disarm attempts failed). One failure reproduces failure mode 3 (bus collision) again, this time on the very first digit of the sequence — the monitor's independent decode shows a bare `Unknown 0xFF` frame in place of the first `KEYPRESS`, rather than a clean ack ever arriving. Combined with the previous session's ENTER and mid-digit collisions, this mechanism now appears able to hit any outgoing keypress in a sequence, not a specific one. The other two failures in this session match already-established signatures (digit-silence, then terminal-key `0x07` silence) with no new mechanism, following (but likely not caused by, since polling had been stable for 4m45s beforehand) a "no ping for 60s" registration-storm.

Separately, this session also produced a malformed 7-byte `ARMED_STATE` (`[11.83.01.46.81.00.80.11]`) decoded by the **interface**, with the monitor's simultaneous independent decode showing nothing of the sort — clean ordinary traffic. This is the *original* 2026-07-12 pairing (ACK-driving interface corrupted, passive monitor clean), the reverse of last session's logs-44/23 pairing (monitor corrupted, interface clean). Both directions have now been observed, in different sessions — at least consistent with general bus noise affecting whichever receiver happens to be unlucky, rather than a mechanism deterministically tied to whichever device drives the hardware ACK, though not conclusive either way.

Failure rate across sessions so far (1/8, 2/6, 3/9, 5/6, 5/6) looks more like a continuum than a "good session"/"bad session" binary — argues for something with continuously-varying severity (e.g. general bus contention level) rather than a single on/off trigger. Not yet established. No code change proposed.

## High failure rate with zero collisions; long-gap cycles reproduce the same two-failures-then-success shape twice (2026-08-05, logs-47/26)

**Source:** `protocol_trace_2026-08-05_disarm_after_arm.md` (full detail) — `esphome-aap-keypad-monitor-logs-47.txt`, `esphome-aap-alarm-interface-logs-26.txt`.

A sixth session, 5 of 9 disarm attempts failed (~56%) — extending the failure-rate continuum (now 1/8, 2/6, 3/9, 5/9, 5/6, 5/6) with no two sessions landing at the same rate. Unlike the previous session, cross-checking every "no ack" failure against the monitor here shows **zero** collision artifacts — every failure is genuine bus silence, meaning a high failure rate doesn't require the collision mechanism to be active. One failure's ack arrived but took 825ms (vs. the usual ~100–200ms) before the watchdog still ran out — the first time a slow-but-present ack has been noted rather than one that's prompt or entirely absent.

This session's ~5-minute-gap cycle reproduces logs-46/25's exact "digit-silence fail, terminal-key-silence fail, success" shape a second time — but with completely healthy `KEYPAD_PING` polling throughout the gap, no registration/ping-loss event at all. This weakens the tentative "registration storm precedes failure" link raised last session, while making the "two failures then success on a long-gap cycle" shape itself a small but now-twice-reproduced pattern worth targeting deliberately in a future capture.

Not yet established. No code change proposed.

## Automatic retry added for CODE_DIGIT_PENDING/CODE_ENTER_PENDING and ARM_AWAY_PENDING/ARM_STAY_PENDING (2026-08-05)

**Rationale:** six independent sessions (logs-10/35, logs-12/37, logs-42, logs-24/25/26) now show, with monitor cross-checks in several of them, that a watchdog timeout in these states reliably means the panel's state did not change — no `ARMED_STATE` broadcast is ever missed by the passive monitor either. That's a materially different situation from the output-select and zone-bypass watchdogs above, where a retry could double-fire an output or undo a bypass toggle that actually landed. Here a blind retry can't undo something that already happened, because nothing did. The worst observed streak needing manual retries before success was 5 consecutive failures (logs-24, logs-42).

**Fix applied:** the arm/disarm watchdog (`crow_alarm_panel.cpp`, "Arm/disarm watchdog") now retries up to `ARM_DISARM_MAX_RETRIES` (5, `crow_alarm_panel.h`) times before falling back to the existing abort behavior (clear state, restore `last_confirmed_acp_state_`). A new `arm_disarm_retry_count_` counter, reset to 0 at the start of every fresh `arm_away()`/`arm_stay()`/`disarm()` call (via `start_code_sequence_()` and the no-code branches), tracks this. On retry:

- `ARM_AWAY_PENDING`/`ARM_STAY_PENDING` just resend `KEY_ARM`/`KEY_STAY`.
- `CODE_DIGIT_PENDING`/`CODE_ENTER_PENDING` restart the whole code+terminal-key sequence from the first digit (`arm_disarm_code_idx_ = 1`, state back to `CODE_DIGIT_PENDING`, digit-ack baseline re-learned) — exactly what every manual retry across every session in this document already did, and which has a 100% eventual success rate in the traces gathered so far.

Because `arm_away()`/`arm_stay()`/`disarm()` all reject a new call while `arm_disarm_state_ != IDLE`, a user's own repeated manual retries (as seen throughout this document) now become no-ops while an automatic retry is already in flight — the entity resolves on its own instead of needing the user to notice the failure and try again by hand.

Compiles and passes `esphome compile` against `crow_alarm_panel_test.yaml`. **Not yet validated on real hardware** — needs a fresh capture (ideally reproducing a multi-failure streak like logs-24/42) to confirm the automatic retries actually land and that no new interaction appears between rapid consecutive retries and the controller (e.g. the same kind of ACK-timing sensitivity documented for output-select's `OUTPUT_SELECT_ENTER_DELAY_MS`).

**Validated on real hardware (2026-08-05, logs-27):** `esphome-aap-alarm-interface-logs-27.txt`, captured with the fix flashed (compiled `14:47:22`). Two disarm cycles: the first succeeds on the first attempt (no retry needed), the second reproduces the familiar "digit-silence/terminal-key-silence, then success" shape already seen manually in logs-44/23, logs-46/25, and logs-47/26 — but fully automatically this time:

```
14:51:46.740  Disarm (single user-initiated call)
14:51:47.349  CMD 0x01 after terminal key, awaiting ARMED_STATE confirmation
14:51:48.288  Arm/disarm: timeout in state 4, retrying (1/5)
14:51:48.669  Arm/disarm: CMD byte changed from 0x01 to 0x07 in CODE_DIGIT_PENDING, continuing
14:51:48.999  CMD 0x07 after terminal key, awaiting ARMED_STATE confirmation
14:51:49.820  Arm/disarm: timeout in state 4, retrying (2/5)
14:51:50.513  [Controller] Disarmed
14:51:50.529  Code sequence: complete (confirmed via ARMED_STATE broadcast)
```

A single `disarm()` call resolved in 3.8s total across two automatic retries, with no user action in between — the retries land cleanly, the CMD-byte-change tolerance from the 2026-07-25 fix keeps working unmodified mid-retry, and there's no sign of any new interaction between the rapid consecutive retries and the controller. Confirms the fix works in practice, not just in code review.

**Further confirmed (2026-08-19, logs-33):** `esphome-aap-alarm-interface-logs-33.txt`, four arm/disarm cycles in a ~14-minute session (`14:06`–`14:20`). All four arms completed cleanly on the first attempt. Two of the four disarms (`14:08:36.196`, `14:20:00.756`) hit the ordinary `CODE_ENTER_PENDING` silence signature — terminal key acked (`CMD 0x07`), then genuine bus silence, 1s watchdog fires `Arm/disarm: timeout in state 4, retrying (1/5)`, full code+terminal sequence resent, succeeds immediately on the retry with no user action. Same shape as the logs-27 validation above, one retry each instead of two, still 100% eventual success. No new failure mechanism, no code change proposed.

## Retry backoff + intent-matched ARMED_STATE resolution (2026-08-22)

**Observed facts** (`traces/home-assistant_2026-08-19T08-03-09.331Z.log` — this episode falls
outside every interface/monitor capture, including logs-33 from earlier the same day): at
`2026-08-19 17:09:17` and `17:09:30`, two consecutive `disarm()` calls each exhausted **all 5
automatic retries** and aborted (`Arm/disarm: timeout in state 4, aborting after 5 retries`) —
12 failed attempts in ~21 s, alternating between state 3 (digit silence) and state 4
(terminal-key silence). A third call at `17:09:46` — ~8 s after the second abort, ~29 s after the
first attempt — succeeded with no retry. The wider context marks this as a controller-side
degradation episode, the strongest evidence yet for the "session-level bad state" theory: the
controller's `KEYPAD_PING` polling stopped entirely around `17:10:55` (the "No ping for 60 s"
storm fires at `17:11:55`), and both physical keypads spontaneously re-registered
(`17:11:16` IP Keypad, `17:11:32` AAP Keypad) — the whole bus lost the controller for a while,
starting ~90 s after the failed disarms. Two near-misses in the same HA logs reinforce the trend:
`2026-08-18 16:17:54` needed 3–4 retries and `2026-08-17 16:47:23` needed 4–5 retries to succeed.

**Inference (medium confidence):** the 2026-08-05 back-to-back retry loop burns its whole budget
in ~7 s (attempt + 1 s watchdog ≈ 1.5 s cadence), well inside a degradation episode that
evidently lasted tens of seconds — while every eventual success in the whole trace record
(manual retries in logs-22/23/24/25/26, and this episode's `17:09:46` recovery) came after
multi-second gaps. Hammering also adds bus contention, the prime suspect for the collision
failure mode (modes documented above).

**Fix applied (1/2 — retry backoff):** the watchdog no longer restarts the sequence immediately
on timeout. It schedules the retry after a growing pause (`ARM_DISARM_RETRY_BACKOFF_MS`:
1/2/3/5/8/13 s for retries 1–6; `ARM_DISARM_MAX_RETRIES` raised 5 → 6), stretching one call's
retry envelope from ~7 s to ~40 s — spanning the ~29 s recovery observed on 2026-08-19. While
the backoff is pending, `KEYPAD_COMMAND`s no longer advance the (dead) sequence — without that
gate, a stray periodic command arriving mid-wait would type leftover digits into the panel
outside any attempt. `send_packet()`'s existing bus-idle wait covers TX timing when the retry
fires. The abort fallback (restore `last_confirmed_acp_state_`) is unchanged.

**Fix applied (2/2 — intent-matched ARMED_STATE resolution from any state):** two related holes
in the previous `CODE_ENTER_PENDING`-only resolution, both made more likely by retries:

- A slow controller can broadcast the outcome of attempt N after the watchdog has already
  restarted the sequence (logs-47/26 recorded an 825 ms ack, right at the 1 s boundary; logs-12/37
  recorded a `Disarmed` broadcast 92 ms *after* an abort). A late `Disarmed` arriving while the
  retry sits in `CODE_DIGIT_PENDING` previously didn't stop the machine — it would keep typing
  the remaining digits + ENTER into a now-disarmed panel, and code+ENTER while disarmed is
  exactly the "arm with code" gesture (logs-42 armed that way), i.e. **the disarm retry could
  re-arm the panel**. Never observed in a trace, but structurally reachable; now closed.
- The registration handshake re-broadcasts the *current* state (logs-46: repeated `Armed Away`
  lines while armed during a re-registration storm). `CODE_ENTER_PENDING` previously accepted
  *any* recognized ARMED_STATE as success, so an `Armed Away` re-announce landing in a disarm's
  `CODE_ENTER_PENDING` window would have falsely completed the disarm while the panel stayed
  armed. Also never directly observed, but the 2026-08-19 episode shows registration storms and
  disarm attempts do co-occur.

Resolution is now intent-matched — a disarm (terminal key `KEY_ENTER`) resolves only on
`Disarmed`; an arm (no-code or code path) resolves on `Arming` or `Armed Away` — and applies
from every non-IDLE state including the retry backoff wait, fully resetting the state machine
(retry counter and pending flag included). Non-matching recognized broadcasts are logged and
ignored by the state machine (entity state is still published from them unconditionally, as
always).

Compiles and passes `esphome config`/`esphome compile` against `crow_alarm_panel_test.yaml`.
**Not yet validated on real hardware** — needs a fresh capture reproducing a multi-failure
streak (ideally another degradation episode like 2026-08-19 17:09) to confirm the backoff
envelope rides it out within a single call, and that the intent-match never wrongly ignores a
genuine confirmation (watch specifically for an arm-stay confirmation shape other than
`Arming`/`Armed Away`, which would now retry instead of resolving — see Open point below).

**Open point:** if the controller broadcasts a distinct "Armed Stay" ARMED_STATE pattern (none
of the three recognized patterns `00.01`/`01.00`/`00.00`), an arm-stay sequence would no longer
resolve via broadcast at all and would fall to the watchdog/retry path, re-pressing KEY_STAY.
The previous code had the same recognition gap (unknown patterns were logged and ignored), so
this is not a regression, but a stay-mode capture confirming the actual broadcast bytes would
settle it.

## Notes

- ARM/STAY/DISARM sequences are simpler than OUTPUT because there's no ACK handshake
- No-code arm_away()/arm_stay() complete after a single KEYPAD_COMMAND (~100ms total)
- Code-based sequences share the same `CODE_DIGIT_PENDING` / `CODE_ENTER_PENDING` states regardless of whether the operation is arm or disarm
- ARMED_STATE messages (0x11) are published independently regardless of `arm_disarm_state_` (entities always reflect them). As of 2026-08-22, an ARMED_STATE broadcast matching the request's intent resolves the sequence from **any** non-IDLE state (including a retry backoff wait) — see "Retry backoff + intent-matched ARMED_STATE resolution" below. `CODE_ENTER_PENDING` remains the only state that *requires* one to succeed (`ARM_AWAY_PENDING`/`ARM_STAY_PENDING` still resolve on their KEYPAD_COMMAND ack).
- All three keypads receive Command broadcasts during arming/disarming; only the originating keypad controls the sequence
- `disarm()` also guards against calling when already disarmed — it returns early if `is_armed()` is false
