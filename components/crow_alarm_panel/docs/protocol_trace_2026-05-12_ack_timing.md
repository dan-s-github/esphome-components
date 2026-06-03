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
