# Crow/AAP Alarm Panel — Bus Wire Format Specification

**Sources:** `traces/20260512.csv`, `traces/20260512v2.csv`, `traces/ESPHome Keypad.txt`,
`traces/IP Keypad.txt`, `traces/esphome-aap-alarm-interface-logs-3.txt`,
`traces/esphome-aap-keypad-monitor-logs-3..9.txt`,
`traces/20260621v1/esphome-aap-keypad-monitor-logs-18.txt`,
`components/crow_alarm_panel/crow_alarm_panel.cpp`

**Confidence key:** High = 5+ independent captures; Medium = 2–4 captures; Low = single observation or code-only.

---

## Physical Layer

| Property | Value | Confidence |
|---|---|---|
| Bus topology | Single shared serial line (party line) | High |
| Clock speed | ~1.2 kHz nominal (833 µs/bit) | High |
| Sampling edge | Falling clock edge (`INTERRUPT_FALLING_EDGE`) | High |
| Bit order | LSB-first (bit 0 arrives first on the wire) | High |
| Idle state | Clock high, data high | High |
| Bus arbitration | None — turn-taking by convention | Medium |
| Glitch filter | Falling edges < 700 µs apart are ignored | High |
| Inter-frame gap | ~28 falling-edge slots after frame end (see `protocol_trace_2026-05-12_ack_timing.md`) | Medium |
| **Hardware ACK** | After the end boundary of any **addressed** frame the addressed keypad drives DAT LOW for ~1 clock cycle (~416–833 µs, 8–13 samples at 19.2 kHz). Broadcast frames produce no ACK (4–5 samples of natural line release only). The controller uses ACK presence to confirm a keypad is alive — absence triggers repeated re-sends and eventually drops the address from its poll cycle. | **High** |

### Hardware ACK detail

```
Clock:  ___↓___↓___↓___↓___↓___↓___↓_______↓___↓___
Data:   ═══╪end boundary═╪ DAT driven LOW  ╪ released
              (ISR fires)  ← ~1 clock cycle →
                            (~416–833 µs)
```

- Addressed frame types that require an ACK: **0x14, 0x15, 0x1D, 0x23** (all per-keypad frames).
- Broadcast frame types produce no ACK: **0x10, 0x11, 0x12, 0x50, 0x54**.
- The ACK is driven by the **addressed keypad** (or the controller for keypad-originated frames).
- Without ACK: controller sends up to **10× re-sends**, then **10× KEYPAD_PING flood**, then permanently removes the address from its poll table.
- Implementation: drive DAT OUTPUT LOW on the ISR call that detects the end boundary; release to INPUT on the very next falling-edge ISR call. No busy-wait needed — the clock cadence sets the duration naturally.

---



```cpp
buffer[idx] = (buffer[idx] >> 1) | ((data_bit ? 1 : 0) << 7);
```

Each incoming bit shifts the buffer right and loads into the MSB.
After 8 bits the result is `b7<<7 | b6<<6 | … | b0<<0` — i.e. the first
bit received on the wire ends up in bit 0 (LSB-first).

---

## Frame Format

Frames use HDLC-like `0x7E` boundary markers:

```
┌──────────┬──────────────────────────────────┬──────────┐
│  0x7E    │  type (1B)  │  payload (N bytes)  │  0x7E    │
│ boundary │             │                     │ boundary │
└──────────┴──────────────────────────────────┴──────────┘
```

- The **opening** `0x7E` is detected by tracking a sliding 8-bit window:
  `boundary_buffer = (boundary_buffer << 1) | bit`. When `boundary_buffer == 0x7E`
  and the framer is *not* inside a frame, a new frame begins.
- The **closing** `0x7E` is detected by the same mechanism while inside a frame.
  The closing boundary byte is included in the saved buffer (last byte of `buffer2`)
  but stripped before dispatch: `data = buffer2[1 .. data_length-2]`.
- Frames shorter than 2 bytes are discarded as noise.
- `0x7E` is never legitimately present inside a frame payload. No byte-stuffing
  mechanism has been observed.

**Parser output convention** used in log messages: `[type.data[0].data[1]... (N)]`
where `N = data.size()` (payload bytes, excluding type and closing boundary).

---

## Message Types

### 0x10 — CONTROLLER_STATUS

*Direction:* Controller → keypad broadcast  
*Trigger:* Zone state change or periodic keepalive  
*Min length:* 5 payload bytes

| Offset | Size | Name | Description | Confidence |
|---|---|---|---|---|
| 0 | 1 | `keypad_addr` | Target keypad address | High |
| 1 | 1 | `UNKNOWN_01` | Observed: `0x00`, `0x01` | Low |
| 2 | 1 | `flags` | System status flags (see below) | High |
| 3 | 1 | `UNKNOWN_02` | Always `0x00` | Low |
| 4 | 1 | `UNKNOWN_03` | Always `0x00` | Low |

**`flags` byte values:**

| Value | Meaning |
|---|---|
| `0x80` | Zone activity — one or more zones active/transitioning |
| `0xC1` | Zones clear — system idle, all zones secure |
| `0x82` | Zone activity variant (bit 1 meaning unknown) |
| `0xC3` | Zones-clear variant (bit 1 meaning unknown) |

**Examples:**
```
10 00 00 C1 00 00   → [AAP Keypad] zones_clear
10 00 00 80 00 00   → [AAP Keypad] zone_active_or_transition
```

---

### 0x11 — ARMED_STATE

*Direction:* Controller → broadcast  
*Trigger:* Arm/disarm state change  
*Min length:* 2 payload bytes (4 bytes seen in practice)

| Offset | Size | Name | Description | Confidence |
|---|---|---|---|---|
| 0 | 1 | `armed` | `0x01` = armed_away; `0x00` = other | High |
| 1 | 1 | `arming` | `0x01` = arming (exit delay); `0x00` = other | High |
| 2 | 1 | `UNKNOWN_01` | Always `0x00` | Low |
| 3 | 1 | `UNKNOWN_02` | Always `0x00` | Low |

**State mapping:**

| `armed` | `arming` | State |
|---|---|---|
| `0x00` | `0x01` | Arming (exit delay active) |
| `0x01` | `0x00` | Armed Away |
| `0x00` | `0x00` | Disarmed |

Armed Stay encoding not yet observed. Assumed to use a fourth combination.

**Examples:**
```
11 00 01 00 00   → Arming
11 01 00 00 00   → Armed Away
11 00 00 00 00   → Disarmed
```

---

### 0x12 — ZONE_STATE

*Direction:* Controller → broadcast  
*Trigger:* Zone sensor change; also sent as full broadcast after keypad registration  
*Min length:* 6 payload bytes

| Offset | Size | Name | Description | Confidence |
|---|---|---|---|---|
| 0 | 1 | `broadcast_type` | `0x00` = incremental; `0x01` = full broadcast | Medium |
| 1 | 1 | `active` | Zone active bitmap — bit N-1 = zone N open/triggered | High |
| 2 | 1 | `alarmed` | Zone alarmed bitmap — bit N-1 = zone N in alarm | High |
| 3 | 1 | `bypassed` | Zone bypass bitmap — bit N-1 = zone N bypassed | Medium |
| 4 | 1 | `UNKNOWN_01` | Always `0x00` | Low |
| 5 | 1 | `UNKNOWN_02` | Always `0x00` | Low |

All observations above come from a standard 8-zone ESL-2. Bytes 4–5 have only ever
been seen as `0x00`, which is equally consistent with "reserved/unused" and with
"zones 9–16 active/alarmed bitmap, always empty on 8-zone hardware" — the two
hypotheses are indistinguishable without a 16-zone panel's traces. The C++ parser
optimistically reads bytes 4/5 as a second active/alarmed bank for zones 9–16 (see
`crow_alarm_panel.cpp`'s `ZONE_STATE` handler), but this is unverified and the
bypass bank for zones 9–16 (`UNKNOWN_02` + 1 = offset 6) doesn't even exist in a
6-byte payload, so bypass can never be reported for zones above 8 as currently
parsed. Treat zones 9–16 as untested until traced against real hardware.

**Zone bitmap (applies to `active`, `alarmed`, `bypassed`):**

| Bit (0-indexed) | Zone |
|---|---|
| 0 (`0x01`) | Zone 1 |
| 1 (`0x02`) | Zone 2 |
| 2 (`0x04`) | Zone 3 |
| 3 (`0x08`) | Zone 4 |
| 4 (`0x10`) | Zone 5 |
| 5 (`0x20`) | Zone 6 |
| 6 (`0x40`) | Zone 7 |
| 7 (`0x80`) | Zone 8 |

The full broadcast (`broadcast_type=0x01`) is sent periodically and after
`KEYPAD_REGISTRATION` announces. **Observed behavior:** the `bypassed` field is always
`0x00` in full broadcasts even when zones are actually bypassed — only incremental packets
(`broadcast_type=0x00`) carry accurate bypass state. The `active` and `alarmed` fields
appear reliable in both variants. Code must not update bypass state from full broadcasts.

**Examples:**
```
12 00 01 00 00 00 00   → Zone 1 active
12 00 10 00 00 00 00   → Zone 5 active
12 00 20 00 00 00 00   → Zone 6 active
12 00 00 01 00 00 00   → Zone 1 alarmed (alarm pending)
12 01 00 00 00 00 00   → Full broadcast, all zones clear
12 00 00 00 00 00 00   → All zones clear (incremental)
```

---

### 0x14 — KEYPAD_COMMAND

*Direction:* Controller → per-keypad  
*Trigger:* Every keypress received; arm/disarm state changes; output selection  
*Min length:* 1 payload byte (6 bytes in practice)

This message instructs individual keypads on what to display (LEDs, beep pattern,
backlight). Digit values entered by the user are **not** embedded here — only the
current mode is broadcast.

| Offset | Size | Name | Description | Confidence |
|---|---|---|---|---|
| 0 | 1 | `keypad_addr` | Target keypad address | High |
| 1 | 1 | `display_code` | Keypad display/mode (see table) | High |
| 2 | 1 | `UNKNOWN_01` | Always `0x00` | Low |
| 3 | 1 | `output_state` | Current output relay bitmap (matches `OUTPUT_STATE`) | Medium |
| 4 | 1 | `armed_state` | `0x00` = disarmed; `0x01` = armed or arming | High |
| 5 | 1 | `UNKNOWN_02` | Observed `0x80` (usual), `0x40` (rare) | Low |

**`display_code` values:**

| Code | Context |
|---|---|
| `0x00` | Normal / idle display (seen after one keypress variant) |
| `0x01` | Digit acknowledged — more digits expected |
| `0x04` | Armed/disarmed status display |
| `0x07` | Code accepted during alarm-pending state |
| `0x15` | Return to normal (disarm complete, for initiating keypad) |
| `0x89` | Arming started — begin exit-delay animation |
| `0x9B` | Alarm pending — entry delay active |
| `0xAA` | Exit-delay countdown notification |

**Examples:**
```
14 00 89 00 08 01 80   → [AAP Keypad] arming; output #4 active; armed
14 00 AA 00 08 01 80   → [AAP Keypad] exit delay; output #4 active; armed
14 00 04 00 08 01 80   → [AAP Keypad] armed_away; output #4 active; armed
14 07 15 00 08 00 80   → [IP Keypad] return to normal; output #4 active; disarmed
14 07 01 00 00 00 80   → [IP Keypad] digit acknowledged; no outputs; disarmed
14 07 9B 00 08 01 80   → [IP Keypad] alarm pending
```

---

### 0x15 — KEYPAD_STATE

*Direction:* **Controller → per-keypad** *(confirmed 2026-06-21 — see note below)*
*Trigger:* Post-registration handshake; periodic keepalive (~every 90–120 s per keypad)
*Min length:* 2 payload bytes (5 bytes in practice)

| Offset | Size | Name | Description | Confidence |
|---|---|---|---|---|
| 0 | 1 | `keypad_addr` | Target keypad address | High |
| 1 | 1 | `mode` | `0x00`=normal, `0x02`=installer, `0x03`=programming | High |
| 2 | 1 | `UNKNOWN_01` | Always `0x00` | Low |
| 3 | 1 | `UNKNOWN_02` | Always `0x00` | Low |
| 4 | 1 | `UNKNOWN_03` | Always `0x03` | Low |

**Directionality note:** Earlier versions of this document listed this as keypad → controller.
Hardware testing on 2026-06-21 confirmed the opposite: an ESPHome device that sends *zero*
software responses to type 0x15 (only hardware ACK) remains stably in the controller poll
cycle indefinitely. Additionally, only one 0x15 frame per keypad per cycle appears in bus
monitor captures — if keypads responded in software there would be two. Type 0x15 is therefore
**unidirectional: controller → keypad. The keypad replies with a hardware ACK bit only.**

**Post-registration handshake:** The controller sends exactly **1× type 0x15** to the newly
registered keypad ~400 ms after receiving its `KEYPAD_REGISTRATION` frame, then begins
normal KEYPAD_PING polling ~85 ms later. Without hardware ACK the controller re-sends up to
10× before giving up.

**Examples:**
```
15 00 00 00 00 03   → [AAP Keypad] normal mode
15 07 00 00 00 03   → [IP Keypad] normal mode
15 05 00 00 00 03   → [ESPHome Keypad] normal mode
```

---

### 0x16 / 0x17 / 0x18 — SETTING_VALUE variants

*Direction:* Controller → broadcast  
*Trigger:* Programming/configuration mode  
*Note:* Not observed in any capture — specification is derived from C++ parser only.

| Type | Min len | Field layout |
|---|---|---|
| `0x16` | 5B | `UNKNOWN` `UNKNOWN` `options_flags` `addr_lo` `addr_hi` |
| `0x17` | 4B | `UNKNOWN` `value` `addr_lo` `addr_hi` |
| `0x18` | 5B | `UNKNOWN` `value_hi` `value_lo` `addr_lo` `addr_hi` |

---

### 0x19 — RESPONSE_TIME

*Direction:* Controller → broadcast  
*Note:* Not observed in any capture. Parser logs `(data[1]<<8)|data[2]` as an integer.

---

### 0x1D — OUTPUT_SELECT_ACK

*Direction:* Controller → keypad  
*Trigger:* Controller receives `OUTPUT` keypress; enters output-selection mode for that keypad  
*Min length:* 1 payload byte (5 bytes in practice)

| Offset | Size | Name | Description | Confidence |
|---|---|---|---|---|
| 0 | 1 | `keypad_addr` | Target keypad address | High |
| 1 | 1 | `output_state` | Current output bitmap at time of ACK (can be non-zero) | Medium |
| 2–4 | 3 | `UNKNOWN` | Always `0x00 0x00 0x00` | Low |

The ACK arrives **promptly** after the OUTPUT keypress frame (~28 falling-edge
slots / ~23 ms). The component must send the digit before the ACK-flood window
closes (~50 ms after OUTPUT keypress). See `protocol_trace_2026-05-12_ack_timing.md`
for timing details.

**Examples:**
```
1D 05 00 00 00 00   → [ESPHome Keypad] output-select entered; no output currently active
1D 06 00 08 00 00   → [Control4 Keypad] output-select entered; output #4 currently active
```

---

### 0x20 — MEMORY_EVENT

*Direction:* Controller → broadcast  
*Note:* Not observed in any capture. Parser logs `data[1]` as an event index number.  
*Min length:* 2 payload bytes.

---

### 0x23 — KEYPAD_PING / POLL

*Direction:* Controller → per-keypad  
*Trigger:* Every ~15 seconds (aligned with `CURRENT_TIME` broadcast)  
*Min length:* 1 payload byte (8 bytes in practice)

| Offset | Size | Name | Description | Confidence |
|---|---|---|---|---|
| 0 | 1 | `keypad_addr` | Target keypad address | High |
| 1–7 | 7 | `poll_data` | Two observed variants (see below) | Medium |

**Variant A — standard poll** (observed 20+ times across all sessions):
```
03 8C 02 01 00 23 0E
```

**Variant B** (observed for address 0x00 and 0x02, coincides with invalid RTC):
```
03 44 81 00 80 11 07
```

Variant B payload likely encodes panel status or capability flags; its exact
semantics are unknown. Variant A is the normal operational poll.

**Examples:**
```
23 00 03 8C 02 01 00 23 0E   → [AAP Keypad] standard poll
23 07 03 8C 02 01 00 23 0E   → [IP Keypad] standard poll
```

---

### 0x50 — OUTPUT_STATE

*Direction:* Controller → broadcast  
*Trigger:* Any output relay changes state  
*Min length:* 1 payload byte

| Offset | Size | Name | Description | Confidence |
|---|---|---|---|---|
| 0 | 1 | `output_bitmap` | Bit N-1 = output N active | High |

Output numbering matches the zone bitmap convention (bit 0 = output 1).
Output 1 (external siren) = `0x01`. Output 2 (internal siren) = `0x02`.
Output 3 (gate relay) = `0x04`. Output 4 (garage door relay) = `0x08`.

The single-pulse-per-arm / double-pulse-per-disarm burst seen on RF-remote-triggered
events (see the `0x7C` entry below, and `protocol_investigations.md`) is a vendor-documented,
output-assignable panel feature — "Pendant Arm/Disarm Chirp to Output" (`P50E`–`P53E` in the
ESL-2 manual) — confirmed enabled to Output 1 on this installation, not incidental behavior.
A separate single-pulse-no-chirp-count option also exists (`P54E`–`P57E`, e.g. for triggering
a video recorder); an output pulse that doesn't fit the arm=1/disarm=2 pattern may be this.

**Examples:**
```
50 00   → All outputs off
50 08   → Output 4 active
```

---

### 0x54 — CURRENT_TIME

*Direction:* Controller → broadcast  
*Trigger:* Every ~15 seconds  
*Min length:* 7 payload bytes

| Offset | Size | Name | Description | Confidence |
|---|---|---|---|---|
| 0 | 1 | `day_of_week` | 1=Sun, 2=Mon, 3=Tue, 4=Wed, 5=Thu, 6=Fri, 7=Sat | High |
| 1 | 1 | `minutes_hi` | Minutes since midnight, high byte | High |
| 2 | 1 | `minutes_lo` | Minutes since midnight, low byte | High |
| 3 | 1 | `seconds` | 0–59 | High |
| 4 | 1 | `day` | Day of month, 1–31 | High |
| 5 | 1 | `month` | Month, 1–12 | High |
| 6 | 1 | `year` | 2-digit year (e.g. `0x1A` = 26 → 2026) | High |

**Decoding:**
```
minutes_since_midnight = (minutes_hi << 8) | minutes_lo
hour   = minutes_since_midnight / 60
minute = minutes_since_midnight % 60
```

**Garbage time values** (seconds ≥ 60, day = 0 or > 31, month = 0 or > 12)
occur regularly and are not (only) an unset-RTC symptom. One confirmed cause,
reproduced across multiple sessions: a single spurious bit gets clocked into
the bitstream right after the `seconds` byte, shifting every following byte's
alignment by one position for the rest of the frame — for `day`/`month`/`year`
(none of which ever set bit 7) this is bit-for-bit indistinguishable from each
of those three bytes being doubled. It recurs deterministically once per
minute, at the `seconds = 15` broadcast. See `protocol_investigations.md`
("`CURRENT_TIME` (0x54) periodic bit-corruption glitch") for the full
byte-level analysis. All fields must be validated individually before use,
and corrupted frames should be discarded rather than corrected (a doubled
value can coincidentally land back in a valid range).

A related but distinct variant fails this recovery outright: `day`/`month` corrupted by
a `×4` (not `×2`) multiplier, which still halves to an even number but lands `month` out
of the valid `1–12` range, so the frame is discarded rather than silently mis-recovered.
Confirmed (2026-09-10) as `real_value × 4`, not arbitrary garbage — see
`protocol_investigations.md`'s 2026-09-10 update for the live midnight-crossing capture
that pins this down.

**Verified example:**
```
54 02 00 79 0F 0B 05 1A
→ Monday 2026-05-11 02:01:15
  day_of_week=2(Mon), minutes=0x0079=121→02:01, sec=15, day=11, month=5, year=0x1A=26
```

---

### 0x7C — RF remote button-press event

*Direction:* Unknown source → broadcast (not yet attributable to a keypad address; no `keypad_addr` byte pattern matches the existing keypad-address convention)  
*Trigger:* A button press on one of the official RF remotes — never observed for any physical-keypad or integration-initiated command  
*Min length:* 8 payload bytes  
*Confidence:* Confirmed on the RF-remote attribution and on which button was pressed (vendor manual, cross-checked below against four independent trace-derived signals per button); low on the exact byte-level encoding mechanism

**Manual cross-reference (`ESL-2 Install & Program Manual (E.V).pdf`, pages 17–19, 48–54):** the panel's RF path is a separate plug-in receiver card, the RX-16 MF349, connected via the "ARRI4" cable — the manual notes explicitly that if this cable is unavailable, "you can wire the receiver the same as a keypad," i.e. the receiver rides the same physical clock/data bus as keypads and presumably reuses the same frame format, which is why `0x7C` shows up as a normal-looking frame despite not coming from a keypad. Remotes are enrolled as **Radio Users** in User slots 21–100 — a completely different addressing space from keypad bus addresses — which is *why* `data[0..2]` never matches the keypad-address convention: it isn't a malformed/omitted keypad address, radio users are categorically not keypads. Each physical 4-button pendant is manual-documented as up to **five separate learned radio-user identities**, one per function class, each with its own program-address block and independent "press the button you wish to learn in" enrollment step: Arm-only (`P18E40E`–`P18E49E` for pendants 0–9), Disarm-only (`P18E50E`–`P18E59E`), **Door 2 Control, linked to Output 3** (`P18E60E`–`P18E69E`), **Door 1 Control, linked to Output 4** (`P18E70E`–`P18E79E`), and Panic (`P18E80E`–`P18E89E`). This structurally explains why `data[3..4]` differs per button even on the *same* physical remote (observed below): each button isn't a sub-code of one remote identity, it's independently enrolled as its own radio user. It also independently confirms the output mapping found by trace: "Door 1" (garage, Output 4) and "Door 2" (Output 3 — wired to the user's gate in this installation, but generically named "Door 2" by the panel) match our button 4/button 3 findings exactly. The manual doesn't document `0x7C`'s on-wire byte layout (it's user/installer-facing, not a protocol reference) — the retransmit-pattern and checksum-byte details below remain trace-only.

**Observed facts (2026-09-09, frigate long-term logger, [[project-crow-alarm-protocol-trace]]):** across ~3 days of continuous capture spanning two remote-triggered arm/disarm cycles (confirmed by the user via the remote's distinct chime — one beep on arm, two on disarm), exactly 8 `Unknown [7c...]` lines appeared, and *only* at those two cycles — never at any physical-keypad or integration-initiated arm/disarm event in the same capture.

**Observed facts (2026-09-10 follow-up, scripted two-remote capture, [[project-crow-alarm-protocol-trace]]):** the user ran a deliberate test sequence on both of their RF remotes — button 1 (arm), button 2 (disarm), button 3 ×3 (gate), button 4 ×3 (garage) — repeated once per remote. Each press produces a pair of near-identical `0x7C` lines ~0.5–0.8s apart (the bus's usual same-event retransmit-for-reliability pattern seen elsewhere, e.g. `KEYPAD_COMMAND` bursts). data[0..2] is a fixed per-remote identity tuple — `90 00 36` for remote A, `42 00 C4` for remote B — constant across every button on that remote and never seen from the other remote. data[3..4] identifies the button, but the value is per-remote (not a shared code across remotes):

| Button | Remote A (`90 00 36`) data[3..4] | Remote B (`42 00 C4`) data[3..4] |
|---|---|---|
| 1 — arm | `BC F6` | `7C ED` |
| 2 — disarm | `C0 FA` | `00 FB` |
| 3 — gate | `B4 EE` | `F4 EE` |
| 4 — garage | `84 3E` | `C4 3E` |

```
04:17:41  7c 90 00 36 BC F6 45 DC F6   remote A, arm
04:17:42  7c 90 00 36 BC F6 49 D8 F6   remote A, arm, retransmit
04:17:47  7c 90 00 36 C0 FA 46 CC EE   remote A, disarm
04:17:47  7c 90 00 36 C0 FA 4A C8 EE   remote A, disarm, retransmit
04:17:50  7c 90 00 36 B4 EE 43 7C DD   remote A, gate, press 1
04:17:51  7c 90 00 36 B4 EE 25 7C EE   remote A, gate, press 1, retransmit
04:18:04  7c 90 00 36 84 3E 47 BC CF   remote A, garage, press 1
04:18:05  7c 90 00 36 84 3E 4B B8 CF   remote A, garage, press 1, retransmit

04:18:33  7c 42 00 C4 7C ED 8B B8 EB   remote B, arm
04:18:36  7c 42 00 C4 00 FB 46 CC F5   remote B, disarm
04:18:41  7c 42 00 C4 F4 EE 43 7C F3   remote B, gate, press 1
04:18:53  7c 42 00 C4 C4 3E 47 BC EE   remote B, garage, press 1
```

**Byte-level pattern (observed, semantics not established):**

- data[0..2] — fixed per-remote identity, see above.
- data[3..4] — per-remote-per-button code (see table above). Not a shared code across remotes and not obviously derived from the remote's identity tuple by any simple transform (XOR, bit-reverse, or addition tried, none match) — consistent with a hardware encoder chip whose per-button output happens to differ between physical units, not a documented protocol field.
- data[5..6] — for buttons 1 (arm), 2 (disarm), and 4 (garage), shifts by a small `+N`/`-N` between the initial send and its retransmit (e.g. `45 DC → 49 D8`, `+4`/`-4`; remote B's arm shows `+8`/`-8` over a longer ~0.75s gap) — plausibly a coarse counter ticking during the retransmit gap, roughly consistent in rate across both remotes and both offsets. Button 3 (gate) breaks this pattern on both remotes: data[5] shifts by a much larger step (`43 → 25`, `-30`) while data[6] stays unchanged (`7C → 7C`) — not yet understood why gate's retransmit encodes differently from the other three buttons.
- data[7] — checksum-like trailing byte. For buttons 1, 2, and 4 it stays identical between the initial send and its retransmit; for button 3 (gate) it changes within the pair too, on both remotes — the same buttons/pattern split as data[5..6] above.

**Inference (high confidence on attribution and per-button identification):** four independent buttons on two separate remote units all show the same signature: an exclusive `0x7C` pair, occurring only when that specific button is pressed, on top of the corroborating `OUTPUT_STATE` (`0x50`) evidence below — reproduced identically across two physically distinct remotes rules out coincidence. The `+N`/`-N` vs. gate's different retransmit pattern (medium confidence, mechanism unknown) suggests gate's button encoding on the remote itself works differently from the other three, not a receiver-side artifact, since it's consistent across both remotes.

**Causality (high confidence, resolves the previously-open question):** the first `0x7C` frame of each pair precedes the controller's resulting action (the `ARMED_STATE`/`Disarmed` broadcast, or the `OUTPUT_STATE` pulse) by ~100–150ms in all 8 button presses checked (both remotes × all 4 buttons) — never simultaneous-or-after. `0x7C` is therefore the RF receiver reporting the button press *to* the controller, which then acts on it ~100ms later — a cause, not a parallel echo of an action already taken.

A second, independent signal confirms each button-to-function mapping: `OUTPUT_STATE` (`0x50`) pulses the output that function drives, every time, for every press, on both remotes:

| Button | `OUTPUT_STATE` pulse | Notes |
|---|---|---|
| 1 — arm | Output 1 (`0x01`), once | External siren chirp — see `protocol_investigations.md` 2026-09-09 |
| 2 — disarm | Output 1 (`0x01`), twice | External siren chirp |
| 3 — gate | Output 3 (`0x04`), once per press | Also followed within ~1s by a `ZONE_STATE` "Zone 3 active" transition each time |
| 4 — garage | Output 4 (`0x08`), once per press | Matches the already-documented garage-door relay mapping (`0x50` above) |

**Practical takeaway:** no code change — not a keypad-address-scoped message, doesn't fit any existing entity, and the manual confirms this is fundamental (radio users, not keypads). The button-to-output mapping (arm/disarm → Output 1 siren, gate/"Door 2" → Output 3, garage/"Door 1" → Output 4) is now vendor-confirmed and reusable for any future "what did the remote do" feature. A fifth pendant function, Panic, is documented (`P18E80E`–`P18E89E`, triggers internal+external siren, with immediate/delayed/entry-delay-only variants at `P8E`) but not yet observed on the bus — worth a trace if a panic-button `0x7C` sample becomes available. Still open: the gate-specific retransmit-pattern anomaly in data[5..7], and whether `arm_stay` (if either remote supports it) produces a distinct data[3..4] code.

---

### 0xA0 — KEYPAD_REGISTRATION

*Direction:* Keypad → controller  
*Trigger:* Keypad power-up or reset; also sent by component after boot delay  
*Min length:* 1 payload byte (2–3 bytes in practice)

| Offset | Size | Name | Description | Confidence |
|---|---|---|---|---|
| 0 | 1 | `keypad_addr` | Originating keypad address | High |
| 1 | 1 | `UNKNOWN_01` | `0x00` for physical keypads; matches `keypad_addr` for ESPHome | Low |
| 2+ | var | `UNKNOWN_extra` | Only seen for ESPHome keypad (one additional `0x00`) | Low |

The controller responds to registration by re-sending `ARMED_STATE`, a full
`ZONE_STATE` broadcast, and beginning (or resuming) `KEYPAD_PING` polls for
that address.

**Examples:**
```
A0 00 00         → [AAP Keypad] registration (physical keypad)
A0 07 00         → [IP Keypad] registration (physical keypad)
A0 05 05 00      → [ESPHome Keypad] registration (virtual keypad, addr=0x05)
```

---

### 0xA1 — KEYPRESS

*Direction:* Keypad → controller  
*Trigger:* Key pressed on keypad  
*Min length:* 2 payload bytes

| Offset | Size | Name | Description | Confidence |
|---|---|---|---|---|
| 0 | 1 | `keypad_addr` | Originating keypad address | High |
| 1 | 1 | `key_code` | Key identifier (see table) | High |

**Key codes:**

| Code | Key |
|---|---|
| `0x00`–`0x09` | Digits 0–9 |
| `0x0A` | OUTPUT |
| `0x0B` | MEMORY |
| `0x0D` | ARM |
| `0x0E` | STAY (untested) |
| `0x0F` | BYPASS |
| `0x10` | PROGRAM |
| `0x11` | ENTER |

**Examples:**
```
A1 07 04   → [IP Keypad] Key 4
A1 07 11   → [IP Keypad] ENTER
A1 07 0A   → [IP Keypad] OUTPUT
A1 00 0D   → [AAP Keypad] ARM
```

---

### 0xD2 — MEMORY_CLEAR

*Direction:* Unknown  
*Note:* Type constant defined in code; no parser logic or captures available.

---

## Known Unknowns

| Field | Location | Observed range | Notes |
|---|---|---|---|
| `CONTROLLER_STATUS.UNKNOWN_01` | 0x10 data[1] | `0x00`, `0x01` | Correlated with zone/armed state? |
| `KEYPAD_COMMAND.UNKNOWN_02` | 0x14 data[5] | `0x80` (usual), `0x40` (rare) | Possible parity or flag byte |
| `KEYPAD_STATE.UNKNOWN_03` | 0x15 data[4] | Always `0x03` | Possibly keypad model/version; field name may be misleading now direction is confirmed controller→keypad |
| `KEYPAD_PING` Variant B payload | 0x23 data[1..7] | `03 44 81 00 80 11 07` | Seen with invalid RTC; possible capability flags |
| `OUTPUT_SELECT_ACK` data[1..4] | 0x1D data[1..4] | Mostly `0x00`; data[1] can be `0x08` | data[1] appears to be current output state at ACK time |

---

## Cross-references

| Topic | Document |
|---|---|
| Output-select state machine | `output_select_state_machine.md` |
| Arm/disarm state machine | `arm_disarm_state_machine.md` |
| Keypad behavioral differences | `keypad_protocol_types.md` |
| ACK turn-around timing (output-select) | `protocol_trace_2026-05-12_ack_timing.md` |
| Hardware ACK bus-level discovery | `protocol_trace_2026-05-12_ack_timing.md` (physical layer section) |
| RTC time-setting traces | `protocol_trace_2026-04-12_time_setting.md` |
| Standard protocol comparison | `protocol_standard_comparison.md` |
