# AAP protocol findings from the 2026-08-05 disarm-after-arm traces

Source traces:

- `../../../traces/esphome-aap-keypad-monitor-logs-41.txt` (passive monitor)
- `../../../traces/esphome-aap-keypad-monitor-logs-42.txt` (passive monitor)
- `../../../traces/esphome-aap-keypad-monitor-logs-43.txt` (passive monitor)
- `../../../traces/esphome-aap-alarm-interface-logs-22.txt` (active interface, paired with logs-43)
- `../../../traces/esphome-aap-keypad-monitor-logs-44.txt` (passive monitor, paired with logs-23)
- `../../../traces/esphome-aap-alarm-interface-logs-23.txt` (active interface, paired with logs-44)
- `../../../traces/esphome-aap-keypad-monitor-logs-45.txt` (passive monitor, paired with logs-24)
- `../../../traces/esphome-aap-alarm-interface-logs-24.txt` (active interface, paired with logs-45)
- `../../../traces/esphome-aap-keypad-monitor-logs-46.txt` (passive monitor, paired with logs-25)
- `../../../traces/esphome-aap-alarm-interface-logs-25.txt` (active interface, paired with logs-46)
- `../../../traces/esphome-aap-keypad-monitor-logs-47.txt` (passive monitor, paired with logs-26)
- `../../../traces/esphome-aap-alarm-interface-logs-26.txt` (active interface, paired with logs-47)

Related: `arm_disarm_state_machine.md` — this note adds two more corroborating sessions to
that document's "`CODE_ENTER_PENDING` silence confirmed genuine" investigation (2026-07-25)
and introduces a candidate (but not confirmed) timing variable.

## Decode method

`esphome-aap-keypad-monitor.yaml` sets `logs: crow_alarm_panel: DEBUG`, but logs-41, -42, and
-43 (all captured from the passive monitor device) contain **zero** `[D]`/`[W]` lines — only
the `[I]` boot banner and `ESP_LOGI`-level `Raw bit trace` lines. The monitor binary in these
sessions was compiled 2026-08-02, three days before the interface binary (2026-08-05) used for
logs-22; the per-tag DEBUG override apparently isn't reaching the monitor's compiled/running
log level. This means logs-41/42/43 had to be decoded independently of the device's own
frame parser.

Frames were recovered by simulating `CrowAlarmPanelStore::interrupt()`'s boundary-detection
state machine (`boundary_buffer_`/`inside_`/`num_bits_`, `crow_alarm_panel.cpp`) directly
against the concatenated `Raw bit trace` bit strings — an 8-bit sliding window compared to
`0x7E` marks frame start/end, exactly as the ISR does. Because the bit-trace capture and the
real frame decoder are gated by the same `is_transmitting_`/glitch-filter logic in the ISR, the
recorded bit stream is bit-for-bit what the onboard parser would have seen, including the
device's own outgoing keypresses. The reconstructed frames matched `protocol_wire_format.md`
cleanly across ~140+ frames per file with no ambiguous decodes.

logs-43 (monitor) and logs-22 (interface) were captured concurrently from the same bus and are
cross-referenced below; logs-22 has full `[D]`/`[W]` output, so no manual bit decoding was
needed for that half. Same story for logs-44/logs-23 (still 2026-08-02-compiled monitor
firmware, still no per-tag DEBUG output there): logs-44 was decoded with the same bit-level
simulator where needed to cross-check specific windows against logs-23.

## Session: logs-41 (clean baseline)

One arm/disarm cycle via the ESPHome virtual keypad (`0x05`), no anomalies:

```
09:30:29.172  [ESPHome Keypad] Key ARM pressed
09:30:29.289  ARMED_STATE: Arming
09:30:57.729  ARMED_STATE: Armed Away
09:31:08.405–08.810  [ESPHome Keypad] digits 4-2-8-6, ENTER (disarm code)
09:31:08.925  ARMED_STATE: Disarmed
09:31:09.024  [ESPHome Keypad] Command display=0x15 ("return to normal")
```

Single attempt, no retries — this is the reference shape a clean cycle should have.

## Session: logs-42 (armed via IP Keypad, 5 of 6 disarms fail)

Armed at `09:31:35.147` via the **IP Keypad (`0x07`)** entering code `4-2-8-6` + ENTER directly
(no ARM key) — the same "arm with code" pattern the physical keypads use, not the ESPHome
interface's own `ARM_AWAY_PENDING` path. `ARMED_STATE: Armed Away` confirmed at `09:32:03.712`.

Six subsequent disarm attempts from the ESPHome keypad (`0x05`), only the last succeeds:

| # | Start (gap after Armed Away) | Outcome |
|---|---|---|
| 1 | 09:32:25.013 (+21.3s) | Only 3 of 4 digits sent (4-2-8), then goes silent — no ENTER, no error |
| 2 | 09:32:31.221 (+27.5s) | Digit "4" sent+acked, then a corrupted/garbage frame (<2 bytes) appears — aborts |
| 3 | 09:32:34.498 (+30.8s) | Full 4-2-8-6-ENTER sent; controller replies `display=0x07` twice, no `ARMED_STATE` at all |
| 4 | 09:32:39.516 (+35.8s) | Digit "4" sent+acked, then another corrupted/garbage frame — aborts |
| 5 | 09:32:43.611 (+39.9s) | Full sequence again; `display=0x07` twice, still no `ARMED_STATE` |
| 6 | 09:32:47.810 (+44.1s) | Full sequence; `ARMED_STATE: Disarmed` at 09:32:48.425 — **success** |

Decoded directly from raw bits (representative frame from attempt 3):

```
14.05.07.00.40.01.80   [ESPHome Keypad] Command, display=0x07, armed=1
```

## Session: logs-43 (monitor) / logs-22 (interface) — 1 of 8 disarms fails

Full session with proper `[D]`/`[W]` logging (from logs-22). Eight arm/disarm cycles; every
arm alternates between the ESPHome interface's own `ARM_AWAY_PENDING` path ("Arm away" /
"Arm/stay: CMD received, sequence complete") and the IP Keypad entering code+ENTER directly.
Only cycle 5's disarm fails:

```
10:21:15.216  [IP Keypad] Key ENTER pressed (arm-with-code 4-2-8-6)
10:21:43.544  [Controller] Armed Away
10:21:50.829  Disarm (HA-triggered)
10:21:50.913–51.334  Code sequence: digits 4-2-8-6 sent, all acked display=0x01 (normal)
10:21:51.334  Code sequence: sending terminal key 0x11 (ENTER)
10:21:51.538  [ESPHome Keypad] Command display=0x07 ("code accepted, alarm-pending")
              Code sequence: CMD 0x07 after terminal key, awaiting ARMED_STATE confirmation
10:21:52.357  Arm/disarm: timeout in state 4, aborting   <- no ARMED_STATE ever arrived
10:21:52.468  alarm_control_panel restored to ARMED_AWAY (last-confirmed-state fix, 2026-07-22)
10:21:56.156  Disarm retried
10:21:56.654  Code sequence: sending terminal key 0x11
10:21:56.776  [Controller] Disarmed — success, ~120ms turnaround
```

Armed→disarm-attempt gap for all 8 cycles in this session:

| Cycle | Preceding arm method | Gap | Outcome |
|---|---|---|---|
| 1 | ESPHome ARM key | 9.6s | success |
| 2 | IP Keypad code+ENTER | 11.1s | success |
| 3 | ESPHome ARM key | 10.2s | success |
| 4 | ESPHome ARM key | 10.0s | success |
| **5** | **IP Keypad code+ENTER** | **7.4s** | **fail** |
| 6 | ESPHome ARM key | 10.4s | success |
| 7 | ESPHome ARM key | 37.8s | success |

## Session: logs-44 (monitor) / logs-23 (interface) — cycle 3 fails twice, then succeeds

Five arm/disarm cycles, arming alternating between the ESPHome interface's own ARM key and the
IP Keypad's direct code+ENTER, same as the previous session. Cycle 3's disarm fails twice in a
row before a third attempt succeeds:

```
11:23:37.412  [Controller] Armed Away  (armed via IP Keypad code+ENTER)
11:23:43.786  Disarm (attempt 1)
11:23:44.491  Code sequence: CMD 0x07 after terminal key, awaiting ARMED_STATE confirmation
11:23:45.314  Arm/disarm: timeout in state 4, aborting
11:23:46.947  Disarm (attempt 2)
11:23:47.669  Code sequence: CMD 0x07 after terminal key, awaiting ARMED_STATE confirmation
11:23:48.490  Arm/disarm: timeout in state 4, aborting
11:23:51.166  Disarm (attempt 3)
11:23:51.851  [Controller] Disarmed — success
```

Armed→disarm-attempt gap for all cycles in this session:

| Cycle | Armed via | Gap | Outcome |
|---|---|---|---|
| 1 | IP Keypad code+ENTER | 6.8s | success |
| 2 | ESPHome ARM key | 16.5s | success |
| 3, attempt 1 | IP Keypad code+ENTER | 6.4s | fail |
| 3, attempt 2 | same | 9.5s | fail |
| 3, attempt 3 | same | 13.8s | success |
| 4 | ESPHome ARM key | 18.9s | success |
| 5 | IP Keypad code+ENTER | **5.5s** | success |

Cycle 5's 5.5s gap — shorter than either of cycle 3's failing attempts — disarmed cleanly. This
directly contradicts the gap-timing lead raised from the previous session (see Findings below).

**Independent monitor decode of cycle 3, attempt 2:** logs-23 (interface) decodes this window
completely cleanly — normal `14.05.01...` digit acks, `14.05.07` after ENTER, then
`[ESPHome Keypad] In normal state`, no corruption. But logs-44 (monitor), decoded independently
via the same bit-level simulator, shows a burst of `Unknown 0xFF`/`0xFE` garbage frames at
`11:23:47.330–47.741` — right after the digit-8 ack, in the same window the interface decoded
perfectly. This is the **opposite** of `arm_disarm_state_machine.md`'s 2026-07-12 finding, where
the ACK-driving interface saw the garbage and the passive monitor (which never drives the
hardware ACK) decoded clean. Here the passive monitor is the one garbling while the ACK-driving
interface is clean — this specific instance can't be explained by "our own ACK-release desyncs
our own RX," since the monitor never drives that ACK at all.

## Session: logs-45 (monitor) / logs-24 (interface) — five consecutive disarm failures

Armed at `12:53:58.226` via the ESPHome interface's own ARM key. Six subsequent disarm
attempts, only the sixth succeeds:

| # | Gap from arm | Outcome | Mechanism (per interface `[D]` log, cross-checked against logs-45) |
| --- | --- | --- | --- |
| 1 | 44.5s | fail | Bus collision — see below |
| 2 | 51.3s | fail | Clean `0x07`-after-terminal-key silence (established signature) |
| 3 | 57.3s | fail | No digit ack at all — failure mode 1 ("timeout in digit state") |
| 4 | 60.3s | fail | Bus collision, new variant — see below |
| 5 | 64.7s | fail | Clean `0x07`-after-terminal-key silence (established signature) |
| 6 | 70.9s | success | Clean throughout |

Two further cycles later in the same session (9.6s and 10.8s gaps) both succeed cleanly —
consistent with the gap-timing lead already being dead (see Findings below).

**Attempt 1 — reproduces the documented `[14.A1.05.11]` collision:** the interface logs
`Code sequence: CMD 0x01 after terminal key, awaiting ARMED_STATE confirmation` (an ordinary-
looking `0x01`, not the usual `0x07`). Independently decoding logs-45's raw bits for this exact
window turns up:

```text
[12:54:43.328] type=0x14 KEYPAD_COMMAND data=a10511
```

This is a byte-for-byte match for the `[14.A1.05.11]` garbled frame already documented in
`arm_disarm_state_machine.md`'s "Terminal key lost in bus collision" failure mode: the
controller's own `0x14` type byte wins bus arbitration over ESPHome's simultaneous
`A1.05.11` (KEYPRESS ENTER) transmission, so the real ENTER never reaches the controller. This
is the first time this exact collision signature has been independently reproduced and
confirmed via a passive monitor's raw bits in a fresh session — previously it was inferred from
the interface's own logs alone.

**Attempt 4 — a new collision variant, hitting a digit instead of the terminal key:** the
interface logs digits 4/2/8 acked normally, then digit "6" sent with no further ack before the
1s digit-state timeout. logs-45's independent decode shows why: instead of a clean
`KEYPRESS [a1.05.06]`, the bus produces an abnormally long, malformed frame:

```text
[12:54:58.995-12:55:03.009] type=0x50 OUTPUT_STATE data=000406
```

(`OUTPUT_STATE` is normally 1 payload byte; this one runs ~4 seconds and contains `04`/`06`
fragments consistent with the digit-4 and digit-6 keypresses being merged/garbled together).
Same collision class as attempt 1, but landing on a mid-sequence digit rather than the terminal
key — the first evidence that this collision mechanism isn't specific to ENTER.

## Session: logs-46 (monitor) / logs-25 (interface) — longer session, 3 of 9 disarms fail

Six arm/disarm cycles, 9 total disarm attempts, 3 failures — a "medium" session between the
extremes of logs-22 (1/8) and logs-24 (5/6). Two failures reproduce already-established
signatures; the third extends the bus-collision finding from the previous session.

| Cycle/attempt | Gap from arm | Outcome | Mechanism |
| --- | --- | --- | --- |
| 1 | 26.1s | success | clean |
| 2 | 26.1s | success | clean |
| 3 | 82.8s | success | clean |
| 4 | 66.7s | success | clean |
| 5, attempt 1 | 30.6s | fail | bus collision on the *first* digit — see below |
| 5, attempt 2 | 34.4s | success | clean |
| 6, attempt 1 | 285.8s | fail | genuine digit-ack silence (mode 1) |
| 6, attempt 2 | 288.7s | fail | genuine `0x07`-after-terminal-key silence (established) |
| 6, attempt 3 | 292.0s | success | clean |

**Cycle 5, attempt 1 — collision hits the first digit:** the interface logs `Code sequence: 4
digits, terminal key 0x11` then times out in `CODE_DIGIT_PENDING` with no ack ever logged for
the first digit. logs-46's independent decode shows why: right at that moment the bus produces
a bare `Unknown 0xFF` garbage frame instead of a clean `KEYPRESS [a1.05.04]`. This is the same
collision class documented in `arm_disarm_state_machine.md` (failure mode 3) and reproduced last
session on ENTER and a mid-sequence digit — now confirmed hitting the very first digit too. Any
keypress in the sequence appears equally vulnerable.

**Cycle 6's two failures follow an unusually long idle-armed period.** Between cycle 6 arming
(`13:40:19.202`) and the first disarm attempt (`13:46:12.485`) there's a ~5m50s gap. During it,
the interface loses ping contact with the controller and re-registers repeatedly
(`13:41:25.876`–`13:41:27.209`, "No ping for 60 s, re-sending registration announce" ×9) — each
re-registration triggers a fresh `ARMED_STATE` re-broadcast per the documented registration
handshake, explaining a run of repeated `Armed Away` lines in that window. However, `KEYPAD_PING`
resumes a normal, steady 15s cadence immediately after (confirmed in the log from `13:41:27`
through `13:46:10`) — polling had been stable for 4m45s before the actual failures, so the
registration storm is at most a weak/coincidental precursor, not clearly causal. Both failures
match already-established signatures (mode 1 digit-silence, then the standard `0x07`-after-
terminal-key silence) — no new mechanism. The 285–292s gaps are also, by a wide margin, the
longest in this investigation to still show failures, reinforcing that gap length isn't
predictive at any point on the scale from 5.5s to nearly 5 minutes.

**A malformed `ARMED_STATE` on the interface, invisible to the monitor:** right after cycle 1's
clean disarm, the interface logs `Armed state unknown [11.83.01.46.81.00.80.11 (7)]` — a 7-byte
payload where `ARMED_STATE` is normally 4. logs-46's simultaneous independent decode shows
nothing of the sort at that timestamp — just ordinary `KEYPAD_PING`/`CURRENT_TIME` traffic, no
`ARMED_STATE` at all. This is the interface's own receiver diverging from ground truth while the
monitor stays clean — the **original** 2026-07-12 pairing from `arm_disarm_state_machine.md`
(ACK-driving interface corrupted, passive monitor clean), the reverse of what last session's
logs-44/23 showed. Both pairings have now been observed across different sessions.

## Session: logs-47 (monitor) / logs-26 (interface) — 5 of 9 disarms fail, no new collisions

Four arm/disarm cycles, 9 total disarm attempts, 5 failures (~56%) — the highest rate seen
outside the two 5/6 sessions, but this time cross-checking against the monitor turns up no
collision artifacts at all; every failure is genuine bus silence.

| Cycle/attempt | Gap from arm | Outcome |
| --- | --- | --- |
| 1, attempt 1 | 17.6s | fail — genuine digit-ack silence |
| 1, attempt 2 | 23.2s | fail — genuine `0x07`-after-terminal-key silence |
| 1, attempt 3 | 30.0s | success |
| 2 | 8.5s | success |
| 3, attempt 1 | 13.2s | fail — `0x07`-after-terminal-key silence, ack itself took 825ms to arrive |
| 3, attempt 2 | 17.1s | success |
| 4, attempt 1 | 299.2s | fail — genuine digit-ack silence |
| 4, attempt 2 | 305.1s | fail — genuine `0x07`-after-terminal-key silence |
| 4, attempt 3 | 311.5s | success |

**The two digit-ack-silence failures (cycle 1/1, cycle 4/1) show no hidden collision this
time.** Cross-checking both against logs-47's independent decode: the keypress goes out
cleanly, gets acked, and then the *next* digit's keypress goes out cleanly too — but its ack
simply never arrives, with no garbled/`Unknown` frame anywhere nearby. Unlike logs-46/25's
first-digit failure (a bare `Unknown 0xFF` in place of the keypress itself), these are genuinely
silent at the ack stage, not TX collisions. The interface's own log can't distinguish the two
cases — cross-checking the monitor is the only way to tell which failure a given "no ack"
timeout actually is.

**Cycle 3, attempt 1 — an unusually slow (not missing) ack:** the terminal key was sent at
`13:32.844`; the `display=0x07` ack didn't arrive until `13:33.669` — an 825ms turnaround,
several times slower than the typical ~100–200ms seen everywhere else in this investigation —
and the watchdog still ran out shortly after with no `ARMED_STATE` ever following. Not a new
failure signature, but the slow ack itself is a data point that hasn't been called out before.

**Cycle 4's ~5-minute gap repeats last session's exact shape, without the registration
storm.** Like logs-46/25's cycle 6, this cycle sits armed for a long stretch (299–311s) before
disarming, and fails twice (digit-silence, then terminal-key silence) before succeeding on the
third attempt — but this time `KEYPAD_PING` polling stayed perfectly healthy the whole time, with
no "no ping for 60s" event anywhere in the gap. This weakens the already-tentative link between
the registration storm and failure from last session, but the "two failures, then success, on a
long-gap cycle" shape has now repeated identically twice.

**The apparent "mystery" extra `Disarmed` broadcast (`14:12:45.044`, no local disarm attempt
nearby) turns out to be nothing new:** it's the documented post-`KEYPAD_REGISTRATION`
re-announce (the IP Keypad re-registers at `14:12:45.027` immediately before it), preceded by a
~500ms burst of `Unknown [ff.]` corruption on the interface right after the prior successful
disarm (`14:12:39.396`–`39.902`) — the same interface-side corruption signature from the
2026-07-12 entry in `arm_disarm_state_machine.md`, not a new mechanism.

## Findings (observed facts)

1. In every failure across both sessions, the code digits and terminal key were sent and acked
   completely normally (`display=0x01` per digit, matching successful cycles bit-for-bit) — the
   failure is not in ESPHome's own TX.
2. The proximate signature is identical to `arm_disarm_state_machine.md`'s 2026-07-25
   "`CODE_ENTER_PENDING` silence confirmed genuine" entry: a clean terminal-key ack
   (`display=0x07` in logs-42/logs-22; no adjacent corruption) followed by genuine bus silence —
   no `ARMED_STATE`, no garbled retry — until the shared 1s watchdog aborts. This is now
   corroborated across two more independent sessions.
3. In logs-43/22, the one failure had the shortest armed→disarm gap (7.4s) of all 8 cycles;
   every other gap (9.6–37.8s) succeeded.
4. In logs-42, this pattern does **not** hold: 5 of 6 failures occurred at gaps of 21–40s, far
   longer than the 7.4s failure in logs-43/22, and only the 6th attempt (44.1s gap) succeeded.
5. logs-44/23 falsifies the gap-timing lead outright: cycle 5's 5.5s gap (the shortest in that
   session) succeeded cleanly, while cycle 3's two failing attempts had longer gaps (6.4s, 9.5s)
   than that success. Gap length is not a usable predictor of failure in any direction.
6. logs-44/23's cycle 3 is also the first session where the *passive monitor* (not the
   ACK-driving interface) shows the `Unknown 0xFF`/`0xFE` RX-corruption signature, in a window
   the interface decoded perfectly — the reverse of the 2026-07-12 pairing in
   `arm_disarm_state_machine.md`, and not explainable by that entry's "our own ACK-release
   desyncs our own RX" mechanism, since the monitor never drives the hardware ACK.
7. In all three sessions, the arm event immediately preceding a failing disarm cycle was
   performed via the physical IP Keypad's direct code+ENTER, not the ESPHome interface's own ARM
   key. But this alone isn't sufficient either — logs-43/22 cycle 2 and logs-44/23 cycles 1 and 5
   were armed the same way and succeeded cleanly. logs-45/24's five-failure cycle breaks this
   pattern entirely: it was armed via the ESPHome interface's own ARM key, not a physical keypad.
8. logs-41/42/43/44 (monitor) show no `[D]`/`[W]` output at all despite `logs: crow_alarm_panel:
   DEBUG` in the yaml, while logs-22/23 (interface, rebuilt 2026-08-05) log normally — consistent
   with the monitor running a stale firmware build (still compiled 2026-08-02 in logs-44/45).
9. logs-45/24 independently reproduces `arm_disarm_state_machine.md`'s documented
   `[14.A1.05.11]` bus-collision failure mode (failure mode 3) via a passive monitor's raw-bit
   decode, and shows a new variant of the same collision class landing on a mid-sequence digit
   keypress instead of the terminal key.
10. logs-45/24's five-failure streak mixes three distinct, previously-catalogued failure
    mechanisms back to back (two bus collisions, two genuine `CODE_ENTER_PENDING` silences, one
    genuine digit-state silence) rather than repeating a single mechanism — consistent with a
    "bad session" raising the odds of every known failure mode together, rather than one new
    mechanism being responsible.
11. logs-46/25 confirms the bus-collision failure mode can hit the *first* digit of a code
    sequence too (a bare `Unknown 0xFF` frame in place of the first `KEYPRESS`), not just ENTER
    or a mid-sequence digit — this mechanism appears equally able to hit any outgoing keypress.
12. logs-46/25's two long-gap (285–292s) failures both match already-established signatures
    (mode 1 digit-silence, then `0x07`-after-terminal-key silence) with no new mechanism, and
    follow — but likely don't causally depend on — a "no ping for 60s" registration-storm that had
    already resolved 4m45s before the failures.
13. logs-46/25 also shows the *original* 2026-07-12 corruption pairing (interface's own decode
    corrupted — a malformed 7-byte `ARMED_STATE` — while the monitor's simultaneous decode is
    clean), the reverse of logs-44/23's pairing. Both directions have now been observed across
    different sessions.
14. logs-47/26's two "no digit ack" failures show no collision artifact at all when
    cross-checked against the monitor — genuinely silent, unlike logs-46/25's first-digit
    failure. The interface's own log cannot distinguish a genuine silence from a hidden
    collision; only a monitor cross-check can.
15. logs-47/26's cycle 3 failure shows an ack that arrived (825ms after the terminal key) but
    much slower than the typical ~100–200ms turnaround seen everywhere else, and the watchdog
    still ran out shortly after — the first time a slow-but-present ack has been noted rather
    than an ack that's either prompt or entirely absent.
16. logs-47/26's ~5-minute-gap cycle repeats the exact "two failures (digit-silence, then
    terminal-key silence), then success" shape from logs-46/25's ~5-minute-gap cycle — but this
    time with zero registration/ping-loss activity in the gap, weakening the tentative link
    between the registration storm and failure raised last session while strengthening the
    "two failures then success" shape itself as a (still small-sample) recurring pattern on
    long-gap cycles specifically.
17. logs-47/26's overall failure rate (5/9, ~56%) extends the failure-rate continuum further:
    1/8, 2/6, 3/9, 5/9, 5/6, 5/6 across sessions so far, with no two sessions landing at exactly
    the same rate.

## Inference (low confidence — six sessions, gap-timing and arming-method leads now both dead)

A short gap between `ARMED_STATE: Armed Away` and the next disarm attempt looked associated with
failure in the logs-43/22 session, but logs-42 and logs-44/23 both kill that lead: logs-42 failed
repeatedly across a much wider range of gaps (21–44s) and only succeeded on the 6th try, and
logs-44/23's shortest gap of the whole session (5.5s, cycle 5) succeeded while two *longer* gaps
in the same session (6.4s, 9.5s, cycle 3) failed. logs-45/24 adds nothing for gap length either
way (all its failures sit at 44–65s, its lone earlier successes at 9.6–10.8s) but does kill the
arming-method lead: its five-failure cycle was armed via the ESPHome interface's own ARM key, not
a physical keypad, breaking the "preceded by IP Keypad code+ENTER" pattern every prior failure
shared. What's left is that **overall session/bus conditions** (contention, an unidentified
controller-side state, or something upstream of any single per-attempt variable) dominate the
failure rate — sessions vary continuously from "good" (1/8 failed, logs-22) through "medium"
(3/9 failed, logs-25; 5/9 failed, logs-26) to "bad" (5/6 failed, twice: logs-42 and logs-24) for
reasons not yet isolated, and a bad-to-medium session appears to raise the odds of multiple
distinct known failure mechanisms simultaneously (collision and silence alike) rather than
swapping in one new mechanism. The bus-collision mechanism itself is now well established (3
independent reproductions across logs-24/25, hitting the terminal key, a mid-sequence digit, and
the first digit) — what's still missing is what makes a session more or less prone to it, and
logs-26 shows collisions aren't a prerequisite for a high failure rate: its 5/9 session had zero
collision artifacts, all genuine silence. Two long-gap (~5 minute) cycles now (logs-25, logs-26)
have independently produced the identical "digit-silence fail, terminal-key-silence fail,
success" shape — still only 2 data points, but a pattern worth watching for.

## Assumption (unverified)

Whether arming via a physical keypad's direct code+ENTER (vs. the ESPHome interface's own ARM
key) makes the controller more likely to respond to a subsequent disarm with `display=0x07`
silence is no longer a live hypothesis — logs-45/24's failure cycle was armed via the ESPHome
interface's own ARM key, contradicting it directly. Also unverified: whether the logs-44/23
monitor-side `0xFF`/`0xFE` corruption, the logs-45/24 `[14.A1.05.11]`-class collisions, and
logs-46/25's interface-side malformed `ARMED_STATE` share a root cause with the interface-side
corruption from the 2026-07-12 entry in `arm_disarm_state_machine.md`, or are distinct phenomena
that happen to produce similar garbled bytes — both the "interface corrupted, monitor clean"
and "monitor corrupted, interface clean" pairings have now been seen, in different sessions,
which is at least consistent with general bus noise hitting either receiver rather than a
mechanism deterministically tied to whichever device drives the hardware ACK, but not
conclusive. Whether "bad sessions" have a detectable common cause (vs. being independently
unlucky) is also unverified — the failure rate across sessions so far (1/8, 2/6, 3/9, 5/9, 5/6,
5/6) looks more like a continuum than two discrete "good"/"bad" buckets, which argues against a
single binary trigger and toward something with a continuous severity (e.g. general bus
contention level). Whether the "registration storm precedes a long-gap failure pair" link from
logs-46/25 is real is now doubtful — logs-47/26's equivalent long-gap cycle showed the identical
failure shape with a completely healthy ping/registration history throughout. A larger sample,
ideally with `ESP_LOGV` raw-frame logging enabled on the interface and the monitor's DEBUG output
actually working, is needed before any of this is treated as more than confounded noise.

## Practical takeaway

- This doesn't change the recommended handling in `CODE_ENTER_PENDING` or `CODE_DIGIT_PENDING` —
  both already resolve correctly via `ARMED_STATE`/watchdog per the 2026-07-12/07-25 redesigns,
  and the 2026-07-22 last-confirmed-state fix means a failed attempt here is cheaply recoverable
  (confirmed again in logs-22 through logs-26: every retry after an abort re-armed the entity
  correctly and the next attempt succeeded, even after five consecutive failures).
- Before spending more effort chasing a root cause, get the monitor device onto current firmware
  so `logs: crow_alarm_panel: DEBUG` actually produces output there — right now half of every
  paired capture (the passive witness) is bit-trace-only, which cost significant manual decode
  effort for logs-41 through logs-47 that the interface's own logging already provides for free.
- The gap-timing and arming-method leads are both spent; the next productive step is probably
  correlating failures against bus-health signals already tracked elsewhere (RX corruption
  bursts, `CURRENT_TIME` glitch timing) or against a session-level "how busy/contended was the
  bus overall" signal, rather than inventing more per-attempt variables. Given logs-26 shows a
  high failure rate with zero collisions, "bus contention" and "collision frequency" may need to
  be tracked as separate signals rather than one combined "bus health" score.

## Open questions

1. Why does the controller sometimes ack a correctly-received ENTER (`display=0x07` or
   otherwise) but never broadcast `ARMED_STATE`? (Carried over, unresolved, from
   `arm_disarm_state_machine.md`'s 2026-07-25 entry — still not established by this data either.)
2. Does the overall per-session failure rate (1/8, 2/6, 3/9, 5/9, 5/6, 5/6 across
   logs-22/23/25/26/42/24) correlate with any bus-health signal visible elsewhere in the same
   captures (e.g. the RX decode corruption documented in `protocol_investigations.md`), or is it
   independent? Worth checking whether the two 5/6 sessions (logs-42, logs-24) share anything
   else (time of day, session duration, zone activity) that logs-26's 5/9 session doesn't.
3. Is the monitor's missing DEBUG/WARN output a stale-firmware artifact, or does the per-tag
   `logs:` override genuinely not take effect for that device? Worth a quick confirmation next
   time both devices are reflashed together.
4. The bus-collision mechanism (failure mode 3 and its variants) is now confirmed able to hit
   any outgoing keypress — terminal key (logs-24), a mid-sequence digit (logs-24), and the first
   digit (logs-25). Is it purely a function of unlucky timing against the controller's own
   periodic broadcasts, or does something about bus timing (e.g. proximity to a periodic
   `KEYPAD_COMMAND`/`KEYPAD_PING` broadcast) predict which specific keypress in a sequence gets
   hit, and does session-level bus contention make it more likely overall in some sessions
   (logs-24, logs-25) than others (logs-22, logs-23, logs-26)? logs-26 shows collisions aren't
   required for a high failure rate, so this may be an independent axis from whatever drives the
   overall rate.
5. Is the "two failures (digit-silence, then terminal-key silence), then success" shape on
   long-gap (~5 minute) cycles — now seen identically in both logs-25 and logs-26 — a real
   pattern specific to long idle periods, or coincidence from a 2-sample base rate? Worth
   specifically targeting a few more long-gap disarms to check.
6. Is the `0xFF`/`0xFE`/malformed-frame corruption seen on the monitor (logs-44) and on the
   interface (logs-46, this session's malformed `ARMED_STATE`) the same underlying mechanism as
   the 2026-07-12 entry in `arm_disarm_state_machine.md`, given it's now been seen hitting
   whichever device *doesn't* drive the hardware ACK as well as the one that does? Needs a
   session with `ESP_LOGV` on both devices simultaneously to compare raw frame-level detail.
