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
Output 4 (garage door relay) = `0x08`.

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

**Verified example:**
```
54 02 00 79 0F 0B 05 1A
→ Monday 2026-05-11 02:01:15
  day_of_week=2(Mon), minutes=0x0079=121→02:01, sec=15, day=11, month=5, year=0x1A=26
```

---

### 0x7C — Unlabeled, likely RF remote arm/disarm event

*Direction:* Unknown source → broadcast (not yet attributable to a keypad address; no `keypad_addr` byte pattern matches the existing keypad-address convention)  
*Trigger (inferred):* An arm/disarm triggered by the official RF remote — not observed for any physical-keypad or integration-initiated arm/disarm  
*Min length:* 8 payload bytes  
*Confidence:* Medium-high on the RF-remote attribution (two independent corroborating signals — see below); low on byte semantics, which remain undecoded

**Observed facts (2026-09-09, frigate long-term logger, [[project-crow-alarm-protocol-trace]]):** across ~3 days of continuous capture spanning two remote-triggered arm/disarm cycles (confirmed by the user via the remote's distinct chime — one beep on arm, two on disarm), exactly 8 `Unknown [7c...]` lines appeared, and *only* at those two cycles — never at any of the several physical-keypad or integration-initiated arm/disarm events in the same capture. Each arm and each disarm produces a pair of near-identical lines ~0.6–0.7s apart (looks like the bus's usual same-event retransmit-for-reliability pattern seen elsewhere, e.g. `KEYPAD_COMMAND` bursts):

```
23:23:41  7c 90 00 36 BC F6 45 DC DE   (arm,    seq 1)
23:23:42  7c 90 00 36 BC F6 49 D8 DE   (arm,    seq 1, retransmit)
23:25:00  7c 90 00 36 C0 FA 46 CC EE   (disarm, seq 1)
23:25:01  7c 90 00 36 C0 FA 4A C8 EE   (disarm, seq 1, retransmit)

02:11:49  7c 90 00 36 BC F6 45 DC BE   (arm,    seq 2)
02:11:50  7c 90 00 36 BC F6 49 D8 BE   (arm,    seq 2, retransmit)
02:12:30  7c 90 00 36 C0 FA 46 CC BE   (disarm, seq 2)
02:12:30  7c 90 00 36 C0 FA 4A C8 BE   (disarm, seq 2, retransmit)
```

**Byte-level pattern (observed, semantics not established):**

- data[0..1] (`90 00`) and data[2] (`36`) are constant across all 8 lines.
- data[3]/data[4] (`BC F6` vs `C0 FA`) distinguish arm from disarm, consistently, in both sequences — `C0 - BC = 4` and `FA - F6 = 4`, the same `+4` step seen elsewhere in this device's fixed-offset fields (e.g. the `CURRENT_TIME` day/month fault set in `protocol_investigations.md`), though here it looks like intentional encoding rather than corruption since it's 100% consistent, not a rare fault.
- data[5]/data[6] (e.g. `45 DC` → `49 D8`) shift by exactly `+4`/`-4` between the initial send and its retransmit ~0.6–0.7s later — plausibly a counter or partial-timestamp field ticking between the two transmissions.
- data[7] (`DE` vs `BE`) is stable across an entire arm+disarm cycle but differs between the two cycles (different calendar dates, ~3h apart) — could be date-dependent, a rolling code/session counter from the remote itself, or something else; not enough samples to distinguish.

**Inference (medium-high confidence on attribution, low confidence on mechanism):** the perfect correlation with remote-triggered events only (0/many at keypad or integration events, 8/8 at exactly the two remote cycles) makes it very likely this packet is specific to whatever hardware handles the RF remote (a receiver module wired into the panel, presumably not enumerated as a normal keypad address) reporting the arm/disarm event it just triggered. A second, independent signal points the same way: the external siren (`OUTPUT_STATE` Output 1 — see `0x50` above) chirps once on arm / twice on disarm with the same 8/8-vs-0 exclusivity to remote-triggered events (`protocol_investigations.md`, 2026-09-09 second follow-up) — two unrelated fields both singling out exactly the same two events is stronger evidence than either alone, even though neither packet's payload obviously *encodes* the 1-vs-2 beep count itself (0x7C sends one arm-flavored pair and one disarm-flavored pair regardless; the siren chirp count is presumably driven by the controller's own arm/disarm logic, not read out of the 0x7C payload). Still open: whether 0x7C is cause (controller reacts to it) or a parallel effect of the same remote-triggered event as the siren chirp.

**Practical takeaway:** no code change — not a keypad-address-scoped message, doesn't fit any existing entity. Worth another remote-triggered capture (ideally 3+ more cycles, and across more calendar dates) to test whether data[7] tracks date, a counter, or something else, and whether the `+4` fields ever take a third value (e.g. for `arm_stay` if the remote supports it, which hasn't been observed yet).

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
