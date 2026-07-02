# AAP ACK/timing findings from 2026-05-12 scope captures

Source traces:

- `../../../traces/20260512.csv`
- `../../../traces/20260512v2.csv`

This note documents timing-related observations from raw `clk/dat` captures taken from working terminals, focused on output-select ACK behavior (`0x1D`) and frame turn-around timing.

## Decode method

The traces were decoded using the same framing assumptions as `CrowAlarmPanelStore::interrupt`:

1. sample data on clock falling edges
2. detect frames by `0x7E` boundaries
3. treat payload as `type + data`

This gives clean, repeatable packet sequences in `20260512v2.csv`, including:

- `a1.07.0a` (Key OUTPUT)
- `1d.07.00.00.00.00` (Output-select ACK)
- `14.07.01.00.00.00.80` (Keypad command)

## Clock cadence from working captures

Across both CSV files:

- most clock high/low run lengths are 4 samples
- most falling-edge intervals are 8 samples
- normal jitter appears at 7 and 9 samples (with smaller tails)

This suggests a stable nominal bit cadence with expected short-term jitter, not perfect edge spacing.

## ACK turn-around sequence (v2 trace)

Two clear output-select interactions are visible in `20260512v2.csv`:

1. `a1.07.0a` -> `1d.07.00.00.00.00` -> `14.07.01.00.00.00.80`
2. `a1.07.0a` -> `1d.07.00.00.00.00` -> `14.07.01.00.00.00.80`

Representative edge windows from the first sequence:

- keypress `0xA1`: `edge[1054-1365]`
- ACK `0x1D`: `edge[1373-2107]`
- command `0x14`: `edge[2115-2794]`

So ACK starts almost immediately after keypress frame completion, and command follows immediately after ACK completion, with only ~8 edge-count gap each time.

## Command/ACK timing (clean output-select sequence)

Using the cleaner output-select sequence in `20260512v2.csv`:

- keypress (OUTPUT): `a1.07.0a` at `edge[23110-23366]`
- ACK: `1d.07.00.00.00.00` at `edge[23605-24059]`
- command: `14.07.01.00.00.00.80` at `edge[24211-24723]`

Measured inter-frame timing from that sequence:

- keypress end -> ACK start: **239 samples** (about **28 falling-edge slots**)
- ACK end -> command start: **152 samples** (about **19 falling-edge slots**)

This confirms the command/ACK exchange is tightly packed in normal operation, with only a short turnaround between frames.

## Protocol/timing implications

1. **Bus-idle gating should include recent clock silence**, not only `data==high`.
   - A short quiet window after the last observed clock edge better matches working turn-around timing.
2. **Falling-edge glitch filtering must tolerate normal jitter.**
   - Overly large minimum edge-interval thresholds risk dropping valid edges.
3. **Keypad TX framing in working traces is boundary-terminated.**
   - Captures show `0x7E ... 0x7E` framing; no reliable evidence that a trailing `0xFE` marker is required for normal keypad-originated traffic.

## Notes

- `20260512.csv` contains additional noisy/partial decodes in places, but still shows the same core clock cadence and periodic `0x23` poll traffic.
- `20260512v2.csv` is the cleaner reference for ACK turn-around behavior.
- a clean `a1 -> 1d -> 14` output-select sequence was not recovered from `20260512.csv` using the same decode assumptions, so timing values above are taken from the v2 capture.

---

## Hardware ACK — bus-level discovery (2026-06-21)

**Source:** `traces/20260621v1/esphome-aap-keypad-monitor-logs-18.txt`, `traces/20260512.csv`

This is distinct from the application-level output-select ACK (0x1D) documented above.
It is a physical bus-level signal produced by the **addressed keypad** immediately after
the end boundary of any frame directed at it.

### Observed behaviour

In the raw CLK+DAT CSV traces, after the end `0x7E` boundary of an addressed frame, the
DAT line stays LOW for **8–13 samples** (~416–677 µs) before rising. Broadcast frames
(0x10, 0x11, 0x12, 0x50, 0x54) show only 4–5 samples of DAT-low (natural line release).
The difference (~1 full clock cycle) is the hardware ACK.

| Frame type | Post-end DAT-low samples | ACK present |
|---|---|---|
| 0x10, 0x11, 0x12, 0x50, 0x54 (broadcast) | 4–5 | No |
| 0x14, 0x15, 0x1D (per-keypad) | 8–13 | Yes |
| 0x23 KEYPAD_PING — absent keypad | 4–5 | No |
| 0x23 KEYPAD_PING — present keypad | ~12 | Yes |

### Consequence of missing ACK

Without the hardware ACK the controller:
1. Re-sends the frame up to **10×** (type 0x15 post-registration handshake observed)
2. Falls back to **10× KEYPAD_PING flood**
3. **Permanently removes** the keypad address from its poll table

Confirmed in `esphome-aap-keypad-monitor-logs-17.txt` (no ACK → dropped) vs
`esphome-aap-keypad-monitor-logs-18.txt` (ACK implemented → 1× type 0x15, stable polling).

### Implementation

Drive DAT OUTPUT LOW at the ISR call that detects the end boundary; release to INPUT on
the very next falling-edge ISR call. This produces exactly one clock cycle of DAT-low
(~416–833 µs) without any busy-wait — the bus clock cadence provides the timing naturally.

See `CrowAlarmPanelStore::interrupt()` in `crow_alarm_panel.cpp` and the
`ack_keypad_address_` / `ack_pending_` fields in `crow_alarm_panel.h`.
