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
controller doesn't confirm in time. Since the retry budget was raised from 5 to 6 (with a
growing 1s–13s backoff), every case observed has resolved successfully within budget
(worst case seen: ~42s, a full 6/6-retry sequence). No root cause has been confirmed — a
`CURRENT_TIME` correlation theory and a bus-corruption/watchdog-proximity theory were each
tested and later ruled out by counter-examples (most recently a retry that occurred during
a full ~27h window with zero corruption or watchdog activity anywhere). The cause remains
genuinely open and is currently a low-priority thread, not an active lead. See
`arm_disarm_state_machine.md` for the full retry investigation history.

**What to do:** nothing — this is the retry logic working as intended. Before the budget
was raised, two `disarm()` calls did exhaust the then-5-retry budget and abort
(`arm_disarm_state_machine.md`, 2026-08-19 entry) — the motivation for the fix. Since the
budget increase to 6, exhaustion (`Arm/disarm: timeout in state %u, aborting after %u
retries`) has not recurred in any capture; if it ever does, that would be worth reporting.

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
during an output-select sequence. The state machine's built-in timeout/retry and
out-of-sequence recovery logic re-synchronizes and completes the sequence normally —
confirmed in a real capture (`protocol_investigations.md`, 2026-09-13 entry), where the
output still switched correctly ~1.4s after the corruption event. (The arm/disarm retries
above look similar but corruption proximity was specifically tested as a cause there and
ruled out by counter-examples — this output-select mechanism is confirmed, arm/disarm's
isn't.)

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

**What to do:** nothing for an isolated line — that's purely cosmetic log noise from
bus-level behavior outside this integration's control. A clustered burst is the same
underlying cause but can occasionally trigger the watchdog re-announce or an output-select
retry documented elsewhere on this page; those are self-recovering too, so still nothing
to act on, just don't be surprised if one of those other entries' symptoms shows up right
after a burst.

---

## Occasional "No ping for 60 s, re-sending registration announce" warning

**What you'll see:** a `WARN`-level log line, at a rate that's varied a lot between
capture windows — roughly 0.05–0.9/h in most windows, with some full 21–27h windows at
zero — followed immediately by the integration re-registering itself on the bus. No
functional interruption — entities keep working normally.

**Cause:** in almost every observed instance, this fires exactly when an
`Unknown [ff.]`/`[fe.]` corruption burst (see above) happens to land in this integration's
own poll slot, so the controller's periodic "ping" is missed for one cycle. The
component's watchdog notices and re-announces, recovering automatically. The panel also
pauses polling for ~15–20s when it raises a fault, which trips the watchdog in the same
way — see "Keypad TROUBLE light / RF interference warning" below. (The 2026-09-25
instance once listed here as unexplained turned out to be one of these.)

Since mid-September 2026 most of these bursts line up with another device on the bus —
the keypad at address `0x07` re-announcing itself every 5 minutes — rather than random
noise; about 1 in 20 of its announces disturbs the bus enough to trigger this. See
`protocol_investigations.md`, "Keypad registration-announce patterns" (2026-10-01).

**What to do:** nothing — this is the existing watchdog recovering exactly as designed.
The observed baseline has varied between capture windows, including at least one full
~21-27h window with zero trips, so treat "roughly hourly" as a loose upper bound rather
than a stable rate; a sustained rate well above that would still be worth a fresh look.

---

## "Never pinged since announce, re-sending registration announce" warning after a power cut

**What you'll see:** right after the alarm panel and this device power up together (power
cut, or the panel being powered down for maintenance), one or more of these `WARN` lines
about a minute apart, then normal polling resumes.

**Cause:** the integration announces itself 15 s after boot. If the panel is still starting up
at that point, it can miss the announce and never poll this device. Observed 2026-10-06, when
the panel was powered down to remove the `0x07` IP module. Before this fix the watchdog only
re-announced after it had been polled at least once, so the device sat unpolled until it was
rebooted by hand. During that time arm/disarm from Home Assistant would not work.

Separately, after both observed panel power-ups (2026-09-15 and 2026-10-06), the controller
also polled an unconfigured address `0x02` for about 5 minutes. Something hardware-ACKed it
during that time. Then 10 rapid unanswered pings ran and `0x02` was dropped. The device behind
`0x02` is not identified. It can't be this integration, which only ACKs its own `address:`.
On 2026-09-15 `0x02` was polled alongside `0x05`. Also on 2026-09-15, the Control4 keypad at
`0x06` went missing from polling until the 2026-10-06 power-up.

**What to do:** nothing — the watchdog now re-announces every 60 s until the controller
starts polling. If it keeps repeating for minutes on end, check that the configured `address:`
is free and that the panel has fully started up.

---

## Occasional "Current time has invalid ..." log lines

**What you'll see:** lines like `Current time has invalid day/month value 36/36` or
`invalid seconds value 90` (logged at `DEBUG`), or `invalid minutes-since-midnight value
1470` (logged at `WARN`, since it means the whole frame's time-of-day is unusable, not
just one field). Not visible anywhere in Home Assistant — the panel's `CURRENT_TIME`
broadcast isn't exposed as an entity.

**Cause:** the bus bit-stuffs (the panel inserts a `0` after five consecutive `1` bits so
the `0x7E` frame marker stays unique) and this component does not yet remove the inserted
bit. Whenever the current time happens to contain such a run, the fields after it are
shifted and read as doubled (or quadrupled). That is why the same values recur on a fixed
schedule rather than randomly: once a minute during half of every ~4¼-hour cycle, at
23:26–23:27 panel-local time every night (the `invalid minutes-since-midnight` WARN, values
1470 and 1503), and all day on the 31st of a month. See `protocol_investigations.md`,
"Bit-stuffing on the wire" (2026-10-01).

**What to do:** nothing — for day/month/year the component tries to recover the known
doubled-bit glitch first (cross-checked against the frame's own weekday field before
trusting the recovery) and only discards the frame if that check fails; other invalid
fields are discarded outright. No entity depends on this data either way, so even the
theoretical edge case where a rarer `×4`-corrupted date could pass the weekday check by
chance (`protocol_wire_format.md`'s `CURRENT_TIME` entry) isn't user-actionable.

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

## Occasional `crow_alarm_panel took a long time for an operation` warning

**What you'll see:** rarely, a `WARN` line like `crow_alarm_panel took a long time for an
operation (91 ms), max is 50 ms`. This is ESPHome core's own loop-timing watchdog (not
specific to this integration in general — it names whichever component's `loop()` call ran
over the scheduler's threshold), so it will show up under this component's name whenever
`crow_alarm_panel`'s `loop()` happens to be the one running long.

**Cause:** first observed 2026-09-17 (2 instances so far). Both coincided with several
separate message-processing paths landing in the same `loop()` tick — once right after a
post-OTA boot (setup, registration announce, and a disarm-state broadcast all at once), once
mid an `Output-select` sequence (an ACK, a `KEYPAD_COMMAND`, and a digit-send all at once).
Neither instance coincided with `ff.`/`fe.` bus corruption or a watchdog re-announce. Too
rare so far to characterize further — see the 2026-09-20 entry in `protocol_investigations.md`.

**What to do:** nothing observed so far — both instances were followed by the in-flight
operation completing normally a moment later. Worth a fresh look if this becomes frequent,
or is ever seen alongside an operation that actually fails rather than just running long.

---

## Keypad TROUBLE light / RF interference warning

**What you'll see:** the physical keypad's TROUBLE indicator comes on, and the fault list
(MEM key) shows the panel's RF interference alarm. In the ESPHome log, around the moment it
starts: status lines ending `trouble:current,latched [10.80.00.C5.04.00]` instead of
the usual `trouble:none [10.00.00.C1.00.00]`, one
`Unknown [70.6E.5A.02]` line, all keypads re-registering, and usually a
"No ping for 60 s" warning about a minute later. Seen twice so far (2026-09-25 and
2026-10-02), both in the early hours.

**Cause:** the panel's own radio-receiver supervision decided its receiver was being
interfered with (manual event `RFIA`). While the fault is current the controller sets
extra bits in its status broadcast, which the optional `trouble` /
`trouble_latched` binary sensors expose (older firmware logged these frames as a
phantom "Keypad 0x80"). The fault restores by itself the next time the
receiver hears an RF remote; the TROUBLE light stays on until someone views the fault on a
keypad or arms the system. What causes the interference isn't known (see
`protocol_investigations.md`, "RF-interference fault (2026-10-03)").

**What to do:** nothing required — press an RF remote button (or wait for the next use) and
view/clear the fault on the keypad. Alarm state and entities are unaffected. If it starts
happening often, check for a new radio source near the panel.

---

## Cross-references

| Topic | Document |
| --- | --- |
| Arm/disarm retry investigation | `arm_disarm_state_machine.md` |
| Bus corruption, watchdog, `CURRENT_TIME` glitches, RF remote findings | `protocol_investigations.md` |
| Message type reference (`0x50` OUTPUT_STATE, `0x54` CURRENT_TIME, `0x7C`, etc.) | `protocol_wire_format.md` |
