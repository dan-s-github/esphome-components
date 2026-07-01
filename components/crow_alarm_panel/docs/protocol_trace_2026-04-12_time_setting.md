# AAP protocol findings from the 2026-04-12 installer/navigation traces

Source traces:

- `../20260412/logs_esphome-aap-keypad-monitor_logs-13.txt`
- `../20260412/logs_esphome-aap-keypad-monitor_logs-14.txt`
- `../20260412/logs_esphome-aap-keypad-monitor_logs-15.txt`
- `../20260412/logs_esphome-aap-keypad-monitor_logs-16.txt`

Related reference:

- `../ESL-2 Install & Program Manual (E.V).pdf`

This note summarizes what the 2026-04-12 traces add beyond `protocol_investigations.md`, with a focus on installer-mode navigation and the `Time&Date` interaction.

## Manual cross-check

The extracted text from `ESL-2 Install & Program Manual (E.V).pdf` lines up with several of the trace-based hypotheses:

1. The LCD keypad has its own built-in menus in program mode.
   - The manual says the programmer can move through the **Main-Menus** with the **Up/Down Arrow** keys and use **Left/Right** arrows at data-entry locations.
   - It also says the LCD keypad defaults to `CLIENT:USER` or `INSTALLER:USER`, then cycles through available menus with Up/Down.
2. Exiting program mode uses `PROG` followed by `ENTER`.
   - The manual says to repeatedly press `<PROG>` until the display shows `"<ENTER> TO EXIT"`, then press `<ENTER>`.
3. `Time&Date` appears to be deeper than the top-level `Users` / `Outputs` style headings.
   - The contents list shows `Users`, `Miscellaneous Panel & Clock Settings`, and `Outputs` as larger sections, with `Setting Time, Date & Daylight Saving` nested under `Miscellaneous Panel & Clock Settings`.

This manual support makes the trace interpretation more credible:

- quiet top-level installer navigation is likely expected, because the keypad itself owns a menu/navigation layer
- a controller-backed packet family may only appear once a specific data-entry subscreen is opened
- the repeated bus-visible `PROGRAM ... ENTER` exit pattern is consistent with the documented LCD keypad workflow

## Addendum from the first-page screen trace

A fourth trace, `../20260412/logs_esphome-aap-keypad-monitor_logs-16.txt`, captures entry/exit from first-page installer screens such as `Users`, `Inputs`, and `Outputs`.

The most useful result is negative evidence: compared with the `Time&Date` traces, these first-page screens do **not** produce any new decoded menu packet family.

Observed behavior:

- installer entry is the same familiar sequence:
  - `PROGRAM 1 0 6 6 ENTER`
  - `In installer mode [15.00.02.00.00.03 (5)]`
  - `Unknown [1f.00.04.00.00.00.00.00.00.00 (9)]`
  - controller status switches to `b3=0x10`
- while the user enters and exits `Users`, `Inputs`, and `Outputs`, there are:
  - no `0x1A`-related packets
  - no `0x19` menu values
  - no `0x17` `Address ...` packets
  - no additional unknown packet types beyond the installer-entry `0x1F`
- exit from installer again appears as:

```text
[13:36:28.156] Key PROGRAM (16) pressed [a1.00.10]
[13:36:28.191] Key PROGRAM (16) pressed [a1.00.10]
[13:36:28.365] Key ENTER (17) pressed [a1.00.11]
[13:36:28.575] [AAP Keypad] In normal state [15.00.00.00.00.03 (5)]
```

### What this suggests

1. The `0x1A` / `0x19` / `0xAB` / `0x26` family is probably specific to editable installer subscreens like `Time&Date`, not generic installer navigation.
2. First-page screen browsing may be handled locally on the keypad until a controller-backed subscreen is opened.
3. The bus-visible `PROGRAM` key continues to look like a likely stand-in for back/cancel navigation inside installer mode.
4. `b3=0x10` still looks like a general installer-context indicator rather than a page-specific value.

This interpretation also matches later trace review: top-level installer screens appear to behave more like local wrappers, while bus-visible controller interaction begins once the user drills into a subscreen that reads or writes panel-backed values.

## Addendum from the clean set-time trace

A second trace, `../20260412/logs_esphome-aap-keypad-monitor_logs-14.txt`, captures a much cleaner set-time-only interaction and sharpens several hypotheses below:

- the first entered group is `1304 ENTER`, matching the controller's visible `13:04` time
- the second entered group is `120426 ENTER`, matching the visible date `2026-04-12` when interpreted as `DDMMYY`
- the final entered group is `1 ENTER`, which plausibly matches the weekday index for Sunday

That makes the installer flow look much more like:

1. enter installer (`PROGRAM 1 0 6 6 ENTER`)
2. edit **time** as `HHMM`
3. edit **date** as `DDMMYY`
4. edit **day-of-week** as a single digit

The clean trace also shows only valid decoded `Controller time update` timestamps during the interaction, which strengthens the idea that the malformed `0x54` frames are a separate variant rather than a problem with the HHMM decode itself.

## Addendum from the settings-navigation trace

A third trace, `../20260412/logs_esphome-aap-keypad-monitor_logs-15.txt`, captures a cleaner walk through:

1. enter installer
2. move to the second settings page
3. open `Time&Date`
4. edit time
5. edit date
6. confirm weekday
7. back out to normal operation

This trace adds several useful clarifications.

### Opening `Time&Date` reuses the same `0x1A` packet family

After installer entry, there is a gap with no additional decoded keypad events for the narrated `down` and `time&date` navigation. The next distinctive packets are the same ones already seen in the earlier traces:

```text
[13:22:33.070] Unknown [ab.00.00.1A.00.01 (5)]
[13:22:33.100] Current time setting received [1321]
[13:22:33.275] Unknown [ab.00.00.1A.00.03 (5)]
[13:22:33.378] Unknown [26.00.0C.04.1A.01.00.00.00.00.1A.03 (11)]
[13:22:33.481] Unknown [ab.00.00.1A.00.02 (5)]
[13:22:33.523] Address 26-2 has value 1 [17.00.01.1A.02]
```

That is strong evidence that the `0x1A`-based packet family is specifically tied to the `Time&Date` settings screen, not just to installer mode in general.

### The first editable field is very likely the live time field

In this trace, the first reported menu value is `1321`, and the controller had just been emitting:

```text
Controller time update: Sunday 2026-04-12 13:21:15
Controller time update: Sunday 2026-04-12 13:21:30
Controller time update: Sunday 2026-04-12 13:21:45
```

The match between `1321` and the visible clock strongly suggests:

- `0x19` is the displayed value for the currently active `Time&Date` field
- the initial active field is the time field

### Confirming the first field immediately applies the new time

The user enters:

```text
1, 3, 0, 4, ENTER
```

and the trace then shows:

```text
[13:22:46.280] Current time setting received [1304]
[13:22:46.382] Controller time update: Sunday 2026-04-12 13:04:01
```

So the first confirmation appears to commit the edited time immediately, before the later date/weekday confirmations complete.

### Field progression after time entry

Right after the time confirmation, the trace moves into the same `0xAB` / `0x26` sequence and then the user enters:

```text
1, 2, 0, 4, 2, 6, ENTER
1, ENTER
```

This reinforces the earlier working interpretation:

- `1304` -> time (`HHMM`)
- `120426` -> date (`DDMMYY`)
- `1` -> weekday

The exact selector mapping is still uncertain, but the cleanest current guess is that the `1A.xx` suffixes identify fields or field transitions within the `Time&Date` editor.

### Installer context persists as `b3=0x10`

While the user navigates the settings UI, controller-status packets keep `b3=0x10`, even when zone activity changes:

```text
[13:22:23.861] ... flags=0x80 ... b3=0x10 ...
[13:22:25.902] ... flags=0xC1 ... b3=0x10 ...
```

This suggests `b3=0x10` is a broader installer/settings context marker, not just a one-shot “installer entered” event.

### “Back” may be represented as `PROGRAM` on the bus

The narrated exit from `Time&Date` / settings is followed by:

```text
[13:22:56.008] Key PROGRAM (16) pressed [a1.00.10]
[13:22:56.053] Key PROGRAM (16) pressed [a1.00.10]
[13:22:56.418] Key ENTER (17) pressed [a1.00.11]
[13:22:56.419] [AAP Keypad] In normal state [15.00.00.00.00.03 (5)]
```

This is a good hint that the keypad may reuse the same bus-visible `PROGRAM` key code for “back” / cancel style UI navigation when operating inside installer menus.

## High-confidence observations

### `0x23` behaves like a keypad keep-alive / ping

The packet currently logged as `Ping` remains highly periodic and keypad-addressed:

```text
[12:52:53.353] [AAP Keypad] Ping [23.00.03.8C.02.01.00.23.0E (8)]
[12:52:53.451] [Control 4 Keypad] Ping [23.06.03.8C.02.01.00.23.0E (8)]
[12:52:53.552] [IP Keypad] Ping [23.07.03.8C.02.01.00.23.0E (8)]
```

In this trace there are 358 ping messages, and they continue through normal operation, installer mode, and time-setting. Nothing here suggests the packet is itself a direct time-setting command.

### `0x10` controller-status decoding is useful, and `b3` changes in installer mode

Normal zone-clear traffic keeps the familiar `flags=0xC1` pattern:

```text
[12:52:47.634] [AAP Keypad] Controller status: ... flags=0xC1 ... b3=0x00 b4=0x00 ...
```

Immediately after entering installer mode, the same status frame changes `b3` from `0x00` to `0x10` while the keypad state changes to installer:

```text
[12:52:53.654] [AAP Keypad] In installer mode [15.00.02.00.00.03 (5)]
[12:52:53.781] [AAP Keypad] Controller status: ... flags=0xC1 ... b3=0x10 b4=0x00 ...
```

This strongly suggests `b3` carries useful context about installer/submenu state and is not always just padding.

### The current time decoder is not suffering from a fixed delta

The longer trace confirms the mixed-frame behavior seen earlier:

- valid examples:
  - `12:25:53 -> Sunday 2026-04-12 12:26:00`
  - `12:26:23 -> Sunday 2026-04-12 12:26:30`
  - `12:26:38 -> Sunday 2026-04-12 12:26:45`
- malformed examples:
  - `12:26:08 -> Sunday 2052-08-24 12:26:27`
  - `12:32:38 -> Sunday 2052-08-24 12:32:89`
  - `12:44:08 -> Sunday 20104-16-48 10:36:31`

So the `data[1:2]` minutes-since-midnight interpretation still looks plausible for the good frames, but some `0x54` frames clearly carry different or malformed date/seconds fields.

## Time-setting interaction

The trace captures a full keypad-driven path into installer mode, repeated time entry, and exit.

### Entering installer mode

The keypad sequence is:

```text
PROGRAM, 1, 0, 6, 6, ENTER
```

Observed on the bus:

```text
[12:52:50.488] [AAP Keypad] Key PROGRAM (16) pressed [a1.00.10]
[12:52:50.583] [AAP Keypad] Key 1 (1) pressed [a1.00.01]
[12:52:51.197] [AAP Keypad] Key 0 (0) pressed [a1.00.00]
[12:52:51.835] [AAP Keypad] Key 6 (6) pressed [a1.00.06]
[12:52:52.528] [AAP Keypad] Key 6 (6) pressed [a1.00.06]
[12:52:53.555] [AAP Keypad] Key ENTER (17) pressed [a1.00.11]
[12:52:53.654] [AAP Keypad] In installer mode [15.00.02.00.00.03 (5)]
```

Two additional packets appear right after installer entry:

```text
[12:52:53.757] Unknown [1f.00.04.00.00.00.00.00.00.00 (9)]
[12:52:53.781] Controller status ... b3=0x10 ...
```

`0x1F` only appears here in this trace, so it is a good candidate for an installer-menu transition packet.

### Repeated “time setting” reads around menu/address `1A`

Once in installer mode, the trace repeatedly shows this packet family:

```text
[12:52:57.034] Unknown [ab.00.00.1A.00.01 (5)]
[12:52:57.046] Current time setting received [1253]
[12:52:57.163] Unknown [ab.00.00.1A.00.03 (5)]
[12:52:57.283] Unknown [26.00.0C.04.1A.01.00.00.00.00.1A.03 (11)]
[12:52:57.444] Unknown [ab.00.00.1A.00.02 (5)]
[12:52:57.453] Address 26-2 has value 1 [17.00.01.1A.02]
```

The same pattern repeats several times during time entry.

## What this likely means

### `0x19` is probably not “response time”

`0x19` is currently named `RESPONSE_TIME`, but in this trace it repeatedly logs:

```text
Current time setting received [1253]
```

That value matches the visible keypad workflow and appears specifically during the time-setting menu, not as a generic timing/latency response. A better working name would be something like:

- `CURRENT_TIME_SETTING`
- `SETTING_TIME_VALUE`
- `MENU_VALUE_16BIT`

The clean trace makes this stronger. Right after entering installer mode it reports:

```text
[13:04:38.378] Current time setting received [1304]
```

That exactly matches the visible controller time in the same capture.

### `0x1A` appears to be the time-setting menu/address

Multiple packets in this workflow reference `1A`:

- `ab.00.00.1A.00.01`
- `ab.00.00.1A.00.02`
- `ab.00.00.1A.00.03`
- `17.00.01.1A.02`
- `26.00.0C.04.1A.01....1A.03`

Working hypothesis:

1. `0x1A` is the installer menu address for the controller time setting.
2. The trailing selector bytes `01`, `02`, and `03` are subfields within that menu item.
3. `0x19` carries the currently displayed HHMM value for that menu item.

The clean trace suggests the surrounding workflow likely extends beyond time-only entry:

- `1304 ENTER` -> likely time (`HHMM`)
- `120426 ENTER` -> likely date (`DDMMYY`)
- `1 ENTER` -> likely day-of-week

The exact meaning of subfields `1A.01`, `1A.02`, and `1A.03` is still unknown.

### `0xAB` and `0x26` are probably menu metadata packets

Based on where they appear, these look related to the currently selected installer item rather than general alarm-state traffic:

- `0xAB` shows up as a short message with `1A` plus a selector (`01`, `02`, `03`)
- `0x26` shows up as a longer aggregate message:

```text
[26.00.0C.04.1A.01.00.00.00.00.1A.03 (11)]
```

Useful working names:

- `0xAB` -> `MENU_FIELD` or `SETTING_FIELD`
- `0x26` -> `MENU_LAYOUT` or `SETTING_FIELD_LIST`

These are hypotheses only, but they fit the observed time-setting flow better than treating them as random unknowns.

## Time entry behavior

The entered digit groups are visible as keypad events:

```text
1 2 5 3 ENTER
1 2 0 4 2 6 ENTER
1 ENTER
```

This sequence repeats twice in the trace, then the user exits with:

```text
PROGRAM, PROGRAM, ENTER
```

Observed exit:

```text
[12:53:18.948] Key PROGRAM (16) pressed [a1.00.10]
[12:53:19.050] Key PROGRAM (16) pressed [a1.00.10]
[12:53:19.066] Key ENTER (17) pressed [a1.00.11]
[12:53:19.357] [AAP Keypad] In normal state [15.00.00.00.00.03 (5)]
```

After exit, the periodic time updates return to their normal-looking cadence.

### Cleaner single-pass sequence

The cleaner trace shows the same workflow with less unrelated activity:

```text
PROGRAM, 1, 0, 6, 6, ENTER
1, 3, 0, 4, ENTER
1, 2, 0, 4, 2, 6, ENTER
1, ENTER
```

This is a much better fit for:

- time = `13:04`
- date = `12/04/26`
- weekday = `1` (likely Sunday)

One useful detail from the clean trace is that the controller starts emitting updated time packets almost immediately after confirmation:

```text
[13:04:46.670] Controller time update: Sunday 2026-04-12 13:04:00
[13:04:48.104] Controller time update: Sunday 2026-04-12 13:04:01
[13:04:48.831] Controller time update: Sunday 2026-04-12 13:04:02
```

That suggests the panel applies the edited values quickly and resets seconds close to the confirmation point.

## Practical protocol improvements suggested by this trace

1. Rename `0x19` from `RESPONSE_TIME` to a time/menu-oriented name.
2. Add first-pass decoders for:
   - `0x1F` installer transition packet
   - `0xAB` menu field packet
   - `0x26` menu field-list / layout packet
3. In `CURRENT_TIME` logging, treat malformed date/seconds combinations as invalid variants instead of presenting them as authoritative controller timestamps.
4. Extend controller-status logging to call out `b3=0x10` as an installer-related context bit/value when `0x15` simultaneously reports installer mode.
5. Consider logging `PROGRAM` as `PROGRAM/BACK` when it occurs inside installer context, or at least document that it may represent back-navigation rather than only the physical Program button.

## Open questions

1. Does `0x19` always represent the currently displayed 16-bit menu value, or is it specific to the HHMM time field?
2. What do `1A.01`, `1A.02`, and `1A.03` correspond to: time, date, weekday, active cursor, or field-transition markers?
3. Is `0x26` describing on-screen editable fields, and does `0xAB` identify the active field?
4. Do the `down` / `time&date` touchscreen navigation actions use a packet family we still are not decoding, or are they handled locally on the keypad until the `Time&Date` editor is entered?
5. Are the malformed `0x54` time frames emitted by the panel itself, or are they a decoding artifact caused by a second time-related packet variant that shares type `0x54`?
