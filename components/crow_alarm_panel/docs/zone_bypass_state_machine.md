# Crow Alarm Panel Zone-Bypass State Machine

## Status: TRACE-VERIFIED

Verified from `esphome-aap-alarm-interface-logs-4.txt` and `esphome-aap-alarm-interface-logs-5.txt`.
These traces capture multiple bypass and unbypass sequences from the physical AAP keypad,
including cumulative bypass across multiple zones.

Additional traces (logs-26 and logs-27) revealed the correct key-sequence timing and the
per-keypad BB semantics. Each bypass is triggered by a single zone key press; the keypad
automatically sends BYPASS + `0` before KC, then zone_digit + ENTER after KC. Logs-26 shows
the failure mode when the panel's session-preservation window has expired (~66 s gap between
bypasses); logs-27 confirms the correct protocol and that consecutive sessions within the
window accumulate existing bypasses.

## Sequences from Traces

### Bypass zone 1 (Entrance) — starting from no zones bypassed

```
 0ms   Key BYPASS (0x0F) pressed [a1.00.0F]
43ms   Key 0 pressed              [a1.00.00]
128ms  Unknown (0x1b)             [1b.00.00.00.00.00.00.00.00.00] ← bypass bitmap = 0x00
209ms  Unknown (0x1b)             [1b.00.00.00.00.00.00.00.00.00]
307ms  KEYPAD_COMMAND             [14.00.01.00.00.00.80]  ← CC=0x01, BB=0x00, flags=0x80
339ms  Key 1 pressed              [a1.00.01]
511ms  Unknown (0x1b)             [1b.00.01.00.00.00.00.00.00.00] ← bypass bitmap = 0x01
544ms  ZONE_STATE broadcast       [00.00.00.01.00.00]     ← bypass byte = 0x01 (zone 1 bypassed)
614ms  KEYPAD_COMMAND             [14.00.01.00.01.00.80]  ← CC=0x01, BB=0x01, flags=0x80
645ms  'Entrance Bypassed' >> ON  (switch published from ZONE_STATE)
819ms  Key ENTER pressed          [a1.00.11]
1023ms KEYPAD_COMMAND             [14.00.15.00.01.00.88]  ← CC=0x15, BB=0x01, flags=0x88
```

### Unbypass zone 1 (Entrance) — toggle when already bypassed

```
 0ms   Key BYPASS (0x0F) pressed [a1.00.0F]
102ms  Unknown (0x1b)             [1b.00.01.00.00.00.00.00.00.00] ← current bitmap = 0x01
111ms  KEYPAD_COMMAND             [14.00.01.00.01.00.88]  ← CC=0x01, BB=0x01, flags=0x88
204ms  Key 0 pressed              [a1.00.00]
308ms  Key 1 pressed              [a1.00.01]
307ms  KEYPAD_COMMAND             [14.00.01.00.01.00.88]  (second command during fast entry)
416ms  Unknown (0x1b)             [1b.00.00.00.00.00.00.00.00.00] ← bitmap = 0x00 (toggled off)
516ms  ZONE_STATE broadcast       [00.00.00.00.00.00]     ← bypass byte = 0x00
620ms  'Entrance Bypassed' >> OFF
620ms  KEYPAD_COMMAND             [14.00.15.00.00.00.80]  ← CC=0x15, BB=0x00, flags=0x80
```

### Add bypass zone 2 (Garage) while zone 2 already bypassed — bypass zone 3 (Garage Door)

```
 0ms   Key BYPASS (0x0F) pressed [a1.00.0F]
 83ms  Unknown (0x1b)             [1b.00.02.00.00.00.00.00.00.00] ← current bitmap = 0x02
167ms  KEYPAD_COMMAND             [14.00.01.00.02.00.88]  ← CC=0x01, BB=0x02, flags=0x88
201ms  Key 0 pressed              [a1.00.00]
270ms  Key 3 pressed              [a1.00.03]
372ms  Unknown (0x1b)             [1b.00.02.00.00.00.00.00.00.00]
400ms  Unknown (0x1b)             [1b.00.06.00.00.00.00.00.00.00] ← bitmap = 0x06 (zones 2+3)
474ms  ZONE_STATE broadcast       [00.00.00.06.00.00]     ← bypass byte = 0x06
577ms  KEYPAD_COMMAND             [14.00.01.00.06.00.88]  ← CC=0x01, BB=0x06, flags=0x88
578ms  'Garage Door Bypassed' >> ON
590ms  Key ENTER pressed          [a1.00.11]
782ms  KEYPAD_COMMAND             [14.00.15.00.06.00.88]  ← CC=0x15, BB=0x06, flags=0x88
```

**Pattern:** BYPASS + `0` (before KC) → [BYPASS_STATUS, BB=current session] → [KEYPAD_COMMAND, BB=current session] → zone_digit + ENTER → [BYPASS_STATUS, BB updated] → [KEYPAD_COMMAND CC=0x15 / any KC]

**Physical user interaction:** A single key press on the zone number. The keypad automatically
generates the entire bus sequence from that one press: BYPASS + `0` sent immediately
(~17–43ms), then after the controller responds with BYPASS_STATUS (~83–103ms) and
KEYPAD_COMMAND (~127–307ms), the keypad sends zone_digit + ENTER back-to-back. No second KC
wait is needed — traces confirm the panel accepts zone_digit + ENTER in the same KC window.
ESPHome replicates this: `KEY_BYPASS` + `digit[0]` are sent in `set_zone_bypass`, then
`digit[1]` + `KEY_ENTER` are sent on the first KC in the `BYPASS_PENDING` handler.

**Session preservation (bypass accumulation):** The BYPASS_STATUS and the first KEYPAD_COMMAND
both carry the **pre-entry bypass bitmap** (the current session state) as their BB byte.
Consecutive bypass sessions within the panel's preservation window (~20–30 s; unconfirmed
upper bound) accumulate: starting a second bypass session shows BB=existing, and pressing
ENTER locks in old + new. Sessions started after the window reset to BB=0x00, wiping
previously bypassed zones when ENTER is pressed (observed in trace-26 at 66 s gap).

**Per-keypad BB:** The BB field in KEYPAD_COMMAND is per-keypad-address. Other keypads on the
bus carry their own BB values (e.g. the IP keypad at address 0x07 always shows BB=0x08). The
BYPASS_PENDING handler therefore filters to our own keypad's KC before advancing.

**ZONE_STATE timing:** The bypass bitmap in `ZONE_STATE` (byte 3) is updated by the controller
**between the 2nd digit and ENTER** — i.e. the switch is published before the sequence fully
completes. This is fast enough that the HA switch shows the new state without perceived lag.

## States

Enum: `ZoneBypassState` (`crow_alarm_panel.h`)

```
IDLE
├─ on set_zone_bypass(zone, state) while disarmed, no other sequence in progress,
│  and reported state != requested state
│  └─> BYPASS_PENDING (KEY_BYPASS + digit[0] sent back-to-back)

BYPASS_PENDING (KEY_BYPASS + first digit sent, waiting for our KEYPAD_COMMAND)
│  Note: the controller first sends BYPASS_STATUS (0x1b) addressed to us before
│  sending KEYPAD_COMMAND. The hardware ACK for 0x1b is handled automatically by
│  the ISR (0x1b is in the addressed-type ACK list); no state transition is needed.
│  KCs from other keypads are ignored (their BB fields are per-keypad and unrelated
│  to our bypass session — e.g. the IP keypad always shows BB=0x08).
├─ on Command(0x14) addressed to our keypad_address
│  └─> ENTER_PENDING (capture BB as zone_bypass_initial_bitmap_; send digit[1] +
│       KEY_ENTER back-to-back; no second KC wait needed — physical keypad sends
│       digit+ENTER within the same KC window)
├─ on timeout (>1s)
│  └─> IDLE (abort, no retry)

ENTER_PENDING (digit[1] + KEY_ENTER sent, waiting for completion)
├─ on Command(0x14) from any keypad (any BB, including CC=0x15)
│  └─> IDLE (sequence done)
├─ on BYPASS_STATUS (0x1b) addressed to us, bitmap CHANGED from zone_bypass_initial_bitmap_
│  └─> IDLE (fallback completion; controller sometimes sends BYPASS_STATUS before
│       KEYPAD_COMMAND CC=0x15, and CC=0x15 may be missed due to hardware-ACK
│       timing overlap)
├─ on BYPASS_STATUS (0x1b) addressed to us, bitmap UNCHANGED
│  └─> stay in ENTER_PENDING (bitmap unchanged means the bypass didn't land;
│       wait for CC=0x15 KEYPAD_COMMAND or timeout)
├─ on timeout (>1s)
│  └─> IDLE (abort, no retry)
```

## KEYPAD_COMMAND (0x14) field meanings during bypass

Format: `[14.KK.CC.00.BB.00.FX]` (6 bytes)

| Field | Offset | Meaning during bypass |
|-------|--------|----------------------|
| KK    | 1      | Keypad address        |
| CC    | 2      | 0x01 = digit-entry mode; 0x15 = ENTER confirmed |
| BB    | 4      | Live bypass bitmap (same as ZONE_STATE byte 3); updates after each digit |
| FX    | 6      | 0x80 = no zones bypassed; 0x88 = ≥1 zone bypassed |

The BB byte tracks the **running bypass state** in real time. After the first digit pair
that toggles a zone, BB already reflects the new bitmap — the panel updates it before ENTER.

## 0x1b packet (BYPASS_STATUS)

```
[1b.00.BB.00.00.00.00.00.00.00]  (10 bytes including checksum)
```

- Byte 2 (`BB`) = current bypass bitmap (same as ZONE_STATE byte 3)
- Appears 1–2 times per bypass sequence:
  - Once immediately after BYPASS key press — carries the **pre-change** bitmap (i.e. the
    current state before any zone is toggled); acts as a status report when the controller
    enters bypass mode
  - Once after the zone digit is processed — carries the **updated** bitmap
- Not a change notification: the first occurrence does not indicate a change has occurred
- **Requires a hardware ACK** (DAT low for one clock cycle) — identical to `KEYPAD_COMMAND`,
  `KEYPAD_STATE`, `OUTPUT_SELECT_ACK`, and `KEYPAD_PING`. Without the ACK the controller
  retries `0x1b` indefinitely and never sends `KEYPAD_COMMAND`, stalling the sequence.
  `BYPASS_STATUS` is included in the ISR's `addressed_type` list to handle this.
- Not observed outside bypass sequences in the captured traces
- Logged as `BYPASS_STATUS` in `crow_alarm_panel.cpp`

## Toggle semantics

The keypad sequence **toggles** the bypass state of a zone; there is no absolute
set-bypass command. Consequences baked into the implementation:

1. `set_zone_bypass(zone, state)` first checks the switch's last reported state. If it
   already matches the request, no sequence is sent. Caveat: the switch defaults to off
   until the first `ZONE_STATE` broadcast, so an "off" request before any broadcast is
   treated as already satisfied.
2. The bypass switch **never publishes optimistically**. Its state is only published from
   the `ZONE_STATE` (0x12) bypass bitmap, which is the source of truth — the switch is
   both the control and the state indicator.
3. The watchdog performs **no automatic retry** on timeout: the timeout does not tell us
   whether the toggle landed, and a blind retry could undo a successful toggle (same
   reasoning as the post-fire OUTPUT-select states).

## Guards on entry

`set_zone_bypass()` refuses to start when:

- a zone-bypass sequence is already in progress;
- the OUTPUT-select or ARM/DISARM machine is non-IDLE — all three machines advance on the
  same `KEYPAD_COMMAND` frames, so concurrent sequences would double-consume confirmations;
- the panel is armed (bypass is assumed to be a disarmed-only operation — **confirmed**:
  all captured bypass sequences occurred while disarmed).

## Confirmation path

Bypass confirmation arrives via `ZONE_STATE` (0x12) byte 3 (bypass bitmap), which the
controller broadcasts **between the 2nd digit and ENTER** — well before the final
KEYPAD_COMMAND. By the time `ENTER_PENDING` completes, the switch is already published.

## Timing Observations

| Event | Duration |
|-------|----------|
| BYPASS key → first KEYPAD_COMMAND | 127–307ms |
| Digit → KEYPAD_COMMAND | 175–307ms |
| KEYPAD_COMMAND → ZONE_STATE broadcast | ~30ms |
| ENTER → final KEYPAD_COMMAND (CC=0x15) | 165–205ms |
| Total sequence (2 digits + ENTER) | ~820ms–1025ms |

## Mapping to Current Code

- `CrowAlarmPanel::set_zone_bypass(zone, state)` — entry point; guards, toggle-skip check,
  digit queue build (`zone / 10`, `zone % 10`), sets `BYPASS_PENDING`, sends `KEY_BYPASS`
  then immediately sends `digit[0]` (the `zone / 10` digit) before yielding to the KC
- Driven in `loop()` under `case KEYPAD_COMMAND:` alongside the other two machines
- Watchdog in `loop()`: >1 s in any non-IDLE state aborts to IDLE without retry
- `CrowAlarmPanelZoneBypassSwitch::write_state()` — delegates to `set_zone_bypass()`;
  never publishes optimistically
- The switch is created either by the parent's `zones:` YAML config (`__init__.py`) or by
  the standalone `switch` platform (`type: bypass`). The class lives in
  `crow_alarm_panel.h/.cpp` rather than `switch/` because parent-created entities must
  compile even when no `switch:` platform entry exists in the user's YAML
