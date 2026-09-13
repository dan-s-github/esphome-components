# Known Quirks

Plain-language descriptions of quirks a user of this integration might actually notice —
in Home Assistant, in the ESPHome logs, or physically at the panel. These are all
already-understood behaviors from the reverse-engineering notes in this `docs/` folder;
each entry links to the detailed technical writeup for anyone who wants the full analysis.

None of these currently need a code change — they're either genuinely benign bus/panel
behavior, or already handled correctly by existing retry/recovery logic. This page exists
so a symptom doesn't get mistaken for a new bug.

---

## Disarming (or arming) sometimes takes a few extra seconds

**What you'll see:** calling `disarm()` (or `arm_away()`/`arm_stay()`) from Home Assistant
usually resolves in well under a second, but occasionally takes several seconds longer —
the alarm control panel entity may sit in a transitional state briefly before landing on
the final one. The log (at `DEBUG`) will show lines like:

```text
[W] Arm/disarm: timeout in state 3, retrying (1/6) in 1000 ms
[D] Arm/disarm: starting retry 1/6
```

**Cause:** the component's retry/backoff logic re-sends the keypress sequence when the
controller doesn't confirm in time. This has always resolved successfully within the
6-retry budget in every case observed so far (worst case seen: ~42s). No single root
cause has been confirmed — a `CURRENT_TIME` correlation theory was tested and later ruled
out; a bus-corruption/watchdog-proximity theory is the current (unconfirmed, single-data-point)
lead. See `arm_disarm_state_machine.md` for the full retry investigation history.

**What to do:** nothing — this is the retry logic working as intended. If a disarm/arm
ever exhausts all 6 retries and gives up (`Arm/disarm: timeout in state %u, aborting after
%u retries`), that would be worth reporting; it has not been observed in any capture so far.

---

## Output changes (e.g. garage door) occasionally retry once before completing

**What you'll see:** calling `set_output()` (garage door, gate, etc. from Home Assistant)
usually completes in well under a second, but occasionally the log shows a couple of
WARN-level lines partway through before it finishes successfully a moment later:

```text
[W] Output-select: timeout in state 3, retrying (output select)
[W] Output-select: KEYPAD_COMMAND in OUTPUT_PENDING (no 0x1D), recovering
```

**Cause:** the same bus-corruption noise documented below (`Unknown [ff.]`/`Unknown [fe.]`)
can occasionally land in the slot where the controller's next confirmation was expected
during an output-select sequence, same as it can for arm/disarm. The state machine's
built-in timeout/retry and out-of-sequence recovery logic re-synchronizes and completes
the sequence normally — confirmed in a real capture (`protocol_investigations.md`,
2026-09-13 entry), where the output still switched correctly ~1.4s after the corruption
event.

**What to do:** nothing — the output still ends up in the correct state. Only worth
reporting if an output-select sequence ever fails outright rather than retrying through.

---

## Occasional `Unknown [ff.]` / `Unknown [fe.]` log lines

**What you'll see:** at `DEBUG` level, occasional lines like `Unknown [ff.]` or
`Unknown [fe.]`, usually in short clustered bursts of several lines within about a second,
roughly once every 1–2 hours in long-term captures.

**Cause:** the controller retransmitting a frame (its own ACK-detection quirk, not
anything this integration does) faster than our receiver can cleanly decode it — the
receiver sees the retransmission burst as corrupted data rather than as a repeated valid
frame. Documented and bit-level-confirmed in `protocol_investigations.md`.

**What to do:** nothing — purely cosmetic log noise from bus-level behavior outside this
integration's control.

---

## Occasional "No ping for 60 s, re-sending registration announce" warning

**What you'll see:** a `WARN`-level log line, roughly every few hours in long-term
captures, followed immediately by the integration re-registering itself on the bus. No
functional interruption — entities keep working normally.

**Cause:** established with no exceptions across many observed instances — this fires
exactly when a `Unknown [ff.]`/`[fe.]` corruption burst (see above) happens to land in
this integration's own poll slot, so the controller's periodic "ping" is missed for one
cycle. The component's watchdog notices and re-announces, recovering automatically.

**What to do:** nothing — this is the existing watchdog recovering exactly as designed.
If it ever happened much more frequently than roughly hourly, that would be worth a fresh
look; the current rate has been stable across many days of capture.

---

## Occasional `WARN`-level "Current time has invalid ..." log lines

**What you'll see:** at `DEBUG`/`WARN` level, occasional lines like `Current time has
invalid day/month value 36/36`, `invalid seconds value 90`, or `invalid
minutes-since-midnight value 1470`. Not visible anywhere in Home Assistant — the panel's
`CURRENT_TIME` broadcast isn't exposed as an entity.

**Cause:** a well-characterized bus-level bit glitch on the `CURRENT_TIME` (`0x54`)
broadcast (a spurious extra bit shifts later fields by one position, indistinguishable
from those fields being doubled). Several specific corrupted values recur consistently
rather than varying randomly, and the minutes-since-midnight variant has recurred at the
same ~2-minute wall-clock window on multiple consecutive days — see the dated
`CURRENT_TIME` sections of `protocol_investigations.md` for the full byte-level analysis.

**What to do:** nothing — the component already discards corrupted `CURRENT_TIME` frames
rather than acting on them, and no entity depends on this data.

---

## Rare truncated-frame warnings (`... too short, discarding` / `... invalid length, discarding`)

**What you'll see:** very occasionally (roughly once every several hours in long-term
captures), a `WARN` line like `Controller status too short, discarding`, `Zone state
invalid length, discarding`, `Output state too short, discarding`, or `Current time too
short, discarding`.

**Cause:** an incomplete/truncated frame reached the parser — likely a byproduct of the
same class of bus-level corruption as the `ff.`/`fe.` lines above. Too rare so far to
characterize further.

**What to do:** nothing — the component correctly discards the malformed frame rather
than acting on partial data.

---

## Cross-references

| Topic | Document |
| --- | --- |
| Arm/disarm retry investigation | `arm_disarm_state_machine.md` |
| Bus corruption, watchdog, `CURRENT_TIME` glitches, RF remote findings | `protocol_investigations.md` |
| Message type reference (`0x50` OUTPUT_STATE, `0x54` CURRENT_TIME, `0x7C`, etc.) | `protocol_wire_format.md` |
