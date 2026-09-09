# Crow Alarm Panel Component

Integration for Arrowhead Crow alarm panels via the two-wire keypad bus (`DAT`/`CLK`).
Registers as a virtual keypad, reads zone and armed state from the controller, and can
send keypresses to arm, disarm, toggle zone bypass, and trigger outputs.

Requires two wires to the panel — `data` and `clock`, usually labelled `DAT` and `CLK`.

---

## Parent platform — `crow_alarm_panel:`

```yaml
crow_alarm_panel:
  clock_pin: GPIO7       # required
  data_pin: GPIO5        # required
  address: 5             # required — keypad bus address (0–7, must be unique on the bus)
  code: !secret alarm_code  # optional — panel-level default disarm code

  keypads:               # optional — labels for other keypads (improves log readability)
    - address: 0
      name: "AAP Keypad"

  zones:                 # optional — auto-create zone sensor + bypass switch per zone
    - zone: 1
      name: "Entrance"
      device_class: motion
      icon: mdi:motion-sensor

  on_message:            # optional — automation trigger for every decoded bus packet
    - lambda: |-
        ESP_LOGI("tag", "%02x - %s", type, format_hex_pretty(data).c_str());
```

### Configuration variables

| Variable | Required | Description |
|----------|----------|-------------|
| `clock_pin` | yes | GPIO connected to the bus CLK line |
| `data_pin` | yes | GPIO connected to the bus DAT line |
| `address` | no | Keypad bus address for this virtual keypad (0–7, must be unique; address 0 is typically the physical keypad). Omit to run in passive monitor mode — see below. |
| `code` | no | Default alarm code used by the disarm button and alarm control panel when no per-entity code is set |
| `keypads` | no | List of `{address, name}` entries to label other keypads in log output |
| `zones` | no | List of zone entries — see [Zone configuration](#zone-configuration) |
| `on_message` | no | Automation trigger fired for every decoded packet; provides `type` (uint8) and `data` (vector of bytes) |

### Passive monitor mode

Omitting `address:` puts the component into passive monitor mode. All bus traffic is
decoded and logged but no packets are sent — the component does not register as a keypad,
does not acknowledge addressed frames, and ignores any arm/disarm/bypass/output requests.

```yaml
crow_alarm_panel:
  clock_pin: GPIO7
  data_pin: GPIO5
  # no address: — passive monitor only

  keypads:
    - address: 0
      name: "AAP Keypad"

  on_message:
    - lambda: |-
        ESP_LOGI("bus", "%02x - %s", type, format_hex_pretty(data).c_str());
```

This is useful for capturing bus traces to understand the protocol before committing to
an address, or for a second ESPHome device that monitors the bus without participating.

---

## Zone configuration

Each entry under `zones:` creates two entities automatically:

- A **zone binary sensor** that turns on when the zone is active
- A **bypass switch** that toggles zone bypass via the `BYPASS → digits → ENTER` keypad
  sequence and mirrors the panel's bypass bitmap (never optimistic)

```yaml
crow_alarm_panel:
  zones:
    - zone: 1                      # required — zone number (1–16)
      device_class: motion         # optional — applied to the zone sensor
      icon: mdi:motion-sensor      # optional — applied to the zone sensor
      # No name: entities are "Zone 1" and "Bypass 1"

    - zone: 2
      name: "Front Door"           # optional — entities become "Front Door" and "Bypass Front Door"
      device_class: door
      # entity_category defaults to "config" for the bypass switch;
      # override to "none" to expose it on dashboards
      entity_category: none
```

### Zone entry variables

| Variable | Required | Default | Description |
|----------|----------|---------|-------------|
| `zone` | yes | — | Zone number (1–16). Only zones 1–8 have been tested against real hardware (a standard 8-zone ESL-2); zones 9–16 rely on an unverified extrapolation of the wire format — see [`protocol_wire_format.md`](docs/protocol_wire_format.md#0x12--zone_state). |
| `name` | no | `"Zone {n}"` | Name for the zone sensor; bypass switch is named `"Bypass {name}"` or `"Bypass {n}"` |
| `device_class` | no | — | Binary sensor device class (e.g. `motion`, `door`, `smoke`) |
| `icon` | no | — | Icon for the zone sensor |
| `entity_category` | no | `config` | Entity category for the bypass switch. Defaults to `config` so bypass switches appear in the HA device Configuration section rather than the main entity list. Set to `none` to expose on dashboards. |

The bypass switch is the sole bypass state indicator — it is published only from the
panel's zone-state bitmap, never set optimistically. The bypass sequence is
`BYPASS` → [controller acknowledgement] → zero-padded zone digits (sent back-to-back)
→ [controller acknowledgement] → `ENTER`.

---

## Child platforms

### `binary_sensor`

Standalone zone sensor for zones not using `zones:`. For bypass state, use the
`switch` platform's `type: bypass` (or the `zones:` parent config) instead — the
bypass switch is both the control and the state indicator.

```yaml
binary_sensor:
  - platform: crow_alarm_panel
    type: zone
    zone: 3
    name: "Back Door"
    device_class: door
```

| Variable | Required | Description |
|----------|----------|-------------|
| `type` | yes | `zone` — active state sensor |
| `zone` | yes | Zone number (1–16) |

All standard binary sensor options (`name`, `device_class`, `icon`, etc.) are supported.

---

### `text_sensor`

Reports the armed state as a human-readable string.

```yaml
text_sensor:
  - platform: crow_alarm_panel
    type: armed_state
    name: "Alarm Status"
```

Published values: `disarmed`, `arming`, `armed_away`, `armed_stay`, `pending`, `triggered`.

---

### `alarm_control_panel`

ESPHome alarm control panel integration. Delegates arm/disarm to the parent's keypress
state machine.

```yaml
alarm_control_panel:
  - platform: crow_alarm_panel
    name: "Crow Alarm"
    code: !secret alarm_code     # optional — overrides parent code for this entity
    requires_code_to_arm: false  # optional — default false
```

| Variable | Required | Default | Description |
|----------|----------|---------|-------------|
| `code` | no | parent `code` | Disarm (and arm, if `requires_code_to_arm: true`) code |
| `requires_code_to_arm` | no | `false` | Require code entry to arm |

---

### `button`

Keypad action buttons.

```yaml
button:
  - platform: crow_alarm_panel
    type: arm_away
    name: "Arm Away"

  - platform: crow_alarm_panel
    type: arm_stay
    name: "Arm Stay"

  - platform: crow_alarm_panel
    type: disarm
    name: "Disarm"
    code: !secret alarm_code   # optional — overrides parent code for this button
```

| `type` | Description |
|--------|-------------|
| `arm_away` | Arms the panel in away mode |
| `arm_stay` | Arms the panel in stay mode |
| `disarm` | Disarms the panel (code required) |

---

### `switch`

Output control, standalone bypass toggle, or raw-frame log toggle.

```yaml
switch:
  - platform: crow_alarm_panel
    type: output
    output: 4
    name: "Garage Door"

  - platform: crow_alarm_panel
    type: bypass
    zone: 3
    name: "Bypass Back Door"

  - platform: crow_alarm_panel
    type: log_raw_frames
    name: "Raw Frame Logging"
```

| Variable | Required | Description |
|----------|----------|-------------|
| `type` | yes | `output` — panel output relay; `bypass` — zone bypass toggle; `log_raw_frames` — runtime raw-frame log toggle |
| `output` | if `type: output` | Output number |
| `zone` | if `type: bypass` | Zone number (1–16) |

The `log_raw_frames` switch is a pure software toggle — no bus traffic, no restored state
across reboots (always starts off). While on, it forces the raw-frame log line ("Received raw
frame [...]") to also log at `INFO` instead of requiring `VERBOSE`, so raw traffic can be
inspected at runtime without recompiling with a higher logger level. Defaults to the
`mdi:text-box-search-outline` icon; set `icon:` to override.

---

## Logging

With `logger:` set to `DEBUG`, the component emits decoded runtime messages for zone
state, armed state, keypad polls, controller time updates, output state, keypad commands,
and bypass status. Unknown or conservatively-interpreted packet meanings are labelled as
such in the log output.

```yaml
logger:
  level: DEBUG
  logs:
    crow_alarm_panel: DEBUG
```

Seeing an occasional `WARN` line or a disarm/arm that takes a few extra seconds? Check
[`docs/known_quirks.md`](docs/known_quirks.md) before assuming it's a new bug — most
recurring log noise from this component is already understood and benign.
