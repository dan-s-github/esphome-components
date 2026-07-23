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
│  └─> IDLE (abort; the display_code changed mid-sequence — anomaly, see logs-6)
├─ on timeout (>1s)
│  └─> IDLE (abort)

CODE_ENTER_PENDING (terminal key sent, waiting for final Command)
├─ on Command(0x14) addressed to us with byte[1] != 0x01
│  └─> IDLE (sequence done; ARMED_STATE 0x11 follows independently)
├─ on Command(0x14) addressed to us with byte[1] == 0x01
│  └─> IDLE (abort; terminal key was lost — bus collision observed in logs-6)
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
7. **Command byte validation:** `CODE_DIGIT_PENDING` does not hardcode the "digit accepted" byte[1] value to 0x01. It learns the value from the first response of each sequence and requires subsequent responses to match that same baseline; a value that *changes* mid-sequence aborts as an anomaly. This was previously hardcoded to require exactly 0x01 (treating any other value, including 0x07, as a rejection) — see "Digit-ack byte is keypad-address-specific, not a validity signal" below for why that broke ESPHome-initiated sequences. `CODE_ENTER_PENDING` **no longer uses byte[1] at all** to judge success or failure — see "CODE_ENTER_PENDING success detection redesigned" (2026-07-12) below for why that was proven unreliable in both directions; success there now comes solely from an independent `ARMED_STATE` broadcast, or failure from the shared 1s watchdog.

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

**2. Controller rejects mid-sequence (`0x07` at digit 3):** The controller begins returning `0x07` instead of `0x01` starting part-way through the code sequence. Without command byte validation the state machine would silently advance through the remaining digits and terminal key, log "Code sequence: complete", but the panel would not arm/disarm. Fixed by aborting on any `data[1] != 0x01` in `CODE_DIGIT_PENDING`.

**3. Terminal key lost in bus collision (`0x01` at ENTER):** ESPHome's terminal-key packet collides with the controller's concurrent periodic KEYPAD_COMMAND. The monitor shows a garbled packet `[14.A1.05.11]` (controller's `0x14` type byte wins bus arbitration but ESPHome's `A1.05.11` payload dominates). The controller never receives ENTER. ESPHome receives the collision's survivor KEYPAD_COMMAND (`0x01`) and mistakes it for the ENTER ACK, declaring the sequence done. Fixed by aborting in `CODE_ENTER_PENDING` when `data[1] == 0x01`.

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

## Notes

- ARM/STAY/DISARM sequences are simpler than OUTPUT because there's no ACK handshake
- No-code arm_away()/arm_stay() complete after a single KEYPAD_COMMAND (~100ms total)
- Code-based sequences share the same `CODE_DIGIT_PENDING` / `CODE_ENTER_PENDING` states regardless of whether the operation is arm or disarm
- ARMED_STATE messages (0x11) are published independently regardless of `arm_disarm_state_` (entities always reflect them). As of 2026-07-12, `CODE_ENTER_PENDING` is the one exception that actively *waits* on one to resolve — see "CODE_ENTER_PENDING success detection redesigned" above. `ARM_AWAY_PENDING`/`ARM_STAY_PENDING`/`CODE_DIGIT_PENDING` still don't gate on it.
- All three keypads receive Command broadcasts during arming/disarming; only the originating keypad controls the sequence
- `disarm()` also guards against calling when already disarmed — it returns early if `is_armed()` is false
