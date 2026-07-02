# Crow Alarm Panel Keypad Protocol Types

## Overview
Analysis of three keypad types (IP, AAP, Control4) reveals significant behavioral differences in ARM/DISARM sequences and OUTPUT control.

---

## Keypad Type Behaviors

### 1. IP Keypad (Address 0x07) — "Smart Keypad"
**Characteristics:**
- Requires security code entry for arm/disarm
- Supports OUTPUT-select sequences
- Provides digit-by-digit feedback via Command messages
- Typical for wall-mounted or remote keypads

**Sequences (from logs-3):**

#### ARM_AWAY with numeric code (sample: 1234)
```
15:56:25.731  Key 1 pressed
15:56:25.846  Key 2 pressed  
15:56:26.037  Key 3 pressed
15:56:26.140  Key 4 pressed
15:56:26.282  Key ENTER pressed
15:56:26.365  → Arming [11.00.01.00.00]
```
**Total duration:** 634ms from first digit to armed state
**Inter-digit timing:** ~110ms, ~191ms, ~103ms, ~142ms (user paced)

#### DISARM with numeric code (sample: 1234)
```
15:57:14.646  Key 1 pressed → Command [14.07.01.00.08.01.80]
15:57:14.774  Key 2 pressed → Command [14.07.01.00.08.01.80]
15:57:14.953  Key 3 pressed → Command [14.07.01.00.08.01.80]
15:57:15.066  Key 4 pressed → Command [14.07.01.00.08.01.80]
15:57:15.260  Key ENTER pressed
15:57:15.362  → Disarmed [11.00.00.00.00]
```
**Total duration:** 716ms
**Inter-digit timing:** ~128ms (1→2), ~179ms (2→3), ~113ms (3→4), ~194ms (4→ENTER)

**Key discovery:** Command payload is IDENTICAL for all digits (`01.00.08.01` = "code-entry mode, output 08")
- Individual digit data is NOT broadcast to other keypads
- Only state indication (code entered, not yet confirmed) is shared
- ENTER changes payload to `07` (enter/confirm code)

#### OUTPUT-select with digit 1
```
15:57:34.806  Key OUTPUT pressed
15:57:35.023  Key 1 pressed       ← 217ms gap (ACK+Command processing)
15:57:35.215  Key ENTER pressed   ← 192ms gap
15:57:35.xxx  Output 01 activated
```
**Pattern:** OUTPUT → [ACK, Command response] → DIGIT → [Command response] → ENTER

---

### 2. AAP Keypad (Address 0x00) — "Direct Control Keypad"
**Characteristics:**
- Supports single-key ARM/DISARM (no code required)
- ARM = Key 13 (0x0D)
- DISARM = Key 13 again
- Can also accept numeric code entry for programmed disarm codes
- Typical for control/automation keypads

**Sequences (from logs-4 and logs-5):**

#### ARM_AWAY — Direct (no code)
```
16:03:10.257  Key ENTER pressed (seems to be a noop)
16:03:10.374  Key ARM (13) pressed  ← 117ms after ENTER
16:03:10.473  → Arming [11.00.01.00.00]  ← 99ms
```
**Total duration:** 216ms (key press to state change)

#### DISARM — Direct (no code, immediately after ARM)
```
16:03:12.925  Key ARM (13) pressed  ← 2.452s after system armed
16:03:13.031  → Disarmed [11.00.00.00.00]  ← 106ms
```
**Total duration:** 106ms

#### ARM_AWAY + later DISARM with numeric code
```
16:05:19.605  Key ENTER pressed
16:05:19.707  Key ARM (13) pressed
16:05:19.809  → Arming [11.00.01.00.00]  ← 204ms
   [57 seconds later, user enters code to disarm]
   
FIRST ATTEMPT (user error — extra digit, code is 5 digits instead of 4):
16:06:23.164  Key 1 pressed
16:06:23.870  Key 2 pressed    ← 706ms (user paced)
16:06:23.972  Key 2 pressed    ← 102ms (DUPLICATE digit, user mistyped!)
16:06:24.317  Key 3 pressed    ← 345ms (user paced)
16:06:24.723  Key 4 pressed    ← 406ms (user paced)
16:06:25.302  Key ENTER pressed ← 579ms
16:06:39.457  → Disarmed [11.00.00.00.00]  ← 14,155ms (!!)
Total code-entry time: 16,293ms

SECOND ATTEMPT (corrected — code is 4 digits: 1234):
16:06:37.899  Key 1 pressed
16:06:38.237  Key 2 pressed    ← 338ms (user paced)
16:06:38.606  Key 3 pressed    ← 369ms (user paced)
16:06:38.841  Key 4 pressed    ← 235ms (user paced)
16:06:39.357  Key ENTER pressed ← 516ms
16:06:39.457  → Disarmed [11.00.00.00.00]  ← 100ms ✓
Total code-entry time: 1,558ms
```

**Key insight:** 
- First attempt took 14+ seconds because user entered 5 digits instead of 4
- Controller waited for 5th digit or timeout + retry
- Second attempt with correct 4-digit code succeeded in ~1.5s (standard ~600ms timing)

---

### 3. Control4 Keypad (Address 0x06)
**Status:** 
Output-select and arm/disarm sequences are now captured (`esphome-aap-keypad-monitor-logs-25.txt`).

**Observations:**
- OUTPUT-select confirmed working: KEY_OUTPUT → digit → KEY_ENTER, same sequence as IP keypad
- ARM (away) is direct: KEY_ARM enters arming state, then delayed armed-away transition
- DISARM requires code entry + ENTER (captured code redacted)
- During arming countdown, keypad receives `Command [14.06.AA.00.00.01.80]` before final armed-away state
- Protocol appears identical to IP keypad for output-select and code-entry command gating
- Address 0x06 confirmed

#### ARM_AWAY + DISARM with code (from logs-25)
```text
07:22:24.859  [Control 4 Keypad] Key ARM (13) pressed [a1.06.0D]
07:22:24.905  → Arming [11.00.01.00.00]
07:22:49.230  [Control 4 Keypad] Command [14.06.AA.00.00.01.80]
07:22:53.121  → Armed Away [11.01.00.00.00]

07:23:09.xxx  Key <digit>
07:23:09.xxx  Key <digit>
07:23:09.xxx  Key <digit>
07:23:10.xxx  Key <digit>
07:23:10.120  Key ENTER
07:23:10.222  → Disarmed [11.00.00.00.00]
```
**Arming delay:** ~28.2s from arming start to armed-away.

---

## Code-Entry Protocol Deep Dive

**Key discovery:** Command packets during code entry are stateless broadcasts.

Each key press sends: `[14.XX.01.00.08.01.80]` (address XX, code "01" = entering code)
- Individual digit values are **NOT** broadcast to other keypads
- Only the state indicator (code-entry mode) is shared
- ENTER key sends: `[14.XX.07.00.08.01.80]` (code "07" = code entered, confirming)

**Timing Summary by Keypad Type:**

| Keypad | Code | Entry Time | Confirm Time | Total |
|--------|------|-----------|------------|--------|
| IP Keypad (0x07) | Sample 1234 | ~610ms | ~100ms | ~710ms |
| AAP Keypad (0x00) | Sample 1234 | ~1,460ms | ~100ms | ~1,560ms |
| AAP Keypad (0x00) | 5-digit (error) | ~2,140ms | 14,155ms ⚠️ | ~16,295ms |
| Control4 (0x06) | Sample (redacted, 4-digit) | ~478ms | ~102ms | ~580ms |
| ESPHome Keypad (0x05) | Sample (redacted, 4-digit) | ~508ms | ~101ms | ~609ms |

The AAP error case (5 digits) shows controller validation timeout:
- Code length mismatch (5 instead of 4 digits)
- Controller waits for 5th digit or times out after ~10s
- Then aborts and returns to normal state
- User can immediately retry with correct code

**No protocol bug here** — system behaved correctly by rejecting invalid-length code.

---

## Key Insights

### ARM/DISARM Protocol Options
1. **Direct ARM (AAP-style):** Single KEY_ARM press (immediate, ~100ms)
2. **Code-based ARM (IP-style):** Enter code then ENTER (~600ms for 4 digits)
3. **Hybrid (AAP):** Can use either direct or code-based

### OUTPUT Sequence Differences
- **IP Keypad:** Supports OUTPUT-select sequences (OUTPUT → DIGIT → ENTER → ACK)
- **AAP Keypad:** May not support OUTPUT-select (only KEY_ARM direct control?)
- **Control4:** Unknown (not yet captured)

### Timing Variations
- **IP Keypad inter-digit:** 103–194ms (user-paced input)
- **AAP Keypad inter-digit:** 102–706ms (user-paced, sometimes excessive gaps)
- **AAP + code disarm issue:** 14+ second disarm suggests code path might be timing out or retrying

### State Broadcast Pattern
All keypads receive:
- **0x14 (Command)** during arm/disarm for state sync across panels
- **0x11 (ARMED_STATE)** when state changes reach stable
- Other keypads see activity but don't interfere with sequence

---

## Implications for ESPHome Keypad (Address 0x05)

Current implementation supports:

1. **Direct arm/disarm** like AAP (single keypress, no code required)
   - `arm_away()` → sends KEY_ARM (→ ARM_AWAY_PENDING state)
   - `arm_stay()` → sends KEY_STAY (→ ARM_STAY_PENDING state)

2. **Code-based arm** like IP (code + terminal key)
   - `arm_away(code)` → code digits + KEY_ARM
   - `arm_stay(code)` → code digits + KEY_STAY

3. **Code-based disarm** (code always required — no no-code toggle path)
   - `disarm(code)` → code digits + KEY_ENTER

4. **OUTPUT-select** like IP (full lock-step state machine)
   - `set_output(n, state)` → KEY_OUTPUT → digits → KEY_ENTER

5. **Multi-keypad state sync**
   - Receives Command broadcasts from other keypads
   - Updates state from ARMED_STATE (0x11) and ZONE_STATE (0x12) messages

---

## Current Status

All three state machines (output-select, arm/disarm direct, arm/disarm code-sequence) are
implemented with Command-gating and 1 s watchdog timeouts.

### Open questions
1. **Confirm `0xAA` semantics** — observed during arming countdown on multiple keypads; likely exit-delay/countdown indicator
2. **Verify `0x7E` in payload** — no payload byte equal to `0x7E` has been observed; unknown whether the protocol reserves this value or byte-stuffing exists for unseen message types
3. **Capture Armed-Stay state encoding** — `0x11` armed_stay variant has not been observed (assumed to use a fourth `armed`/`arming` byte combination)

---

## Trace Files Reference

| File | Keypad | Content |
|------|--------|---------|
| esphome-aap-keypad-monitor-logs-3.txt | IP (0x07) | ARM (redacted code), DISARM (redacted code), OUTPUT(4) |
| esphome-aap-keypad-monitor-logs-4.txt | AAP (0x00) | Direct ARM, immediate DISARM |
| esphome-aap-keypad-monitor-logs-5.txt | AAP (0x00) | Direct ARM, then 57s later code-based DISARM (redacted 5-digit) with timing issues |
| esphome-aap-keypad-monitor-logs-25.txt | Control4 (0x06), ESPHome (0x05) | Control4 ARM + 28s arming delay + code DISARM (redacted); ESPHome ARM + 28s arming delay + code DISARM (redacted) |
