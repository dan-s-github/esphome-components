# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

An ESPHome external-components repository. The only component is `crow_alarm_panel`, an integration for Arrowhead Crow alarm panels via their two-wire (clock/data) keypad bus. Users consume it through ESPHome's `external_components` feature; locally it is exercised the same way via `crow_alarm_panel_test.yaml` (`type: local`, `path: components`).

## Commands

Local development uses `uv` on Python 3.13 (3.14+ lacks PlatformIO wheels).

| Task | Command |
| --- | --- |
| Create/update the environment | `uv sync` |
| Validate the canonical fixture | `uv run esphome config crow_alarm_panel_test.yaml` |
| Compile the canonical fixture | `uv run esphome compile crow_alarm_panel_test.yaml` |
| Lint a commit message | `uv run commitlint "feat: add keypad monitor fixture"` |
| Lint the latest commit | `uv run commitlint --hash HEAD` |
| Lint a commit range | `uv run commitlint --from-hash <base> --to-hash HEAD` |
| Install the commit-msg hook | `uv run pre-commit install --hook-type commit-msg` |

There is no source-code lint target or unit-test suite; ESPHome config validation/compile and commit-message linting (Conventional Commits) are the practical checks. Compiling requires a `secrets.yaml` (wifi credentials, API key, alarm code — see the fixture's `!secret` references).

## Architecture

**Python is schema/codegen glue only; all protocol logic lives in C++.** Bus parsing, timing, queueing, and state transitions belong in `crow_alarm_panel.h` / `crow_alarm_panel.cpp`.

- `components/crow_alarm_panel/__init__.py` is the integration entrypoint: defines the parent `crow_alarm_panel:` schema, declares the `CrowAlarmPanel` / `CrowAlarmControlPanel` C++ classes, `AUTO_LOAD`s the child platforms, sets `MULTI_CONF = True`, and wires the `on_message` trigger.
- `crow_alarm_panel.h/.cpp` — the bus protocol handler. `CrowAlarmPanelStore` captures keypad-bus traffic in GPIO ISRs (including hardware ACK and glitch filtering); `CrowAlarmPanel::loop()` parses completed frames and publishes state to registered entities. Outbound traffic (keypresses, packets) is serialized here behind bus-idle checks and timing guards.
- Three explicit state machines drive multi-step TX sequences: `OutputSelectState` (output switching via OUTPUT → digits → ENTER keypresses), `ArmDisarmState` (arm/disarm, optionally with code digits), and `ZoneBypassState` (bypass toggling via BYPASS → zone digits → ENTER; sequence inferred, not trace-verified). Each step waits for a `KEYPAD_COMMAND` (0x14) confirmation from the controller before sending the next key. Only one machine may run at a time — they all consume the same confirmations. Timing constants (e.g. `OUTPUT_SELECT_ENTER_DELAY_MS`) encode hard-won protocol behavior — read the comments and `docs/` before changing them.
- The parent `zones:` config auto-creates a zone binary sensor and a bypass toggle switch per zone (`_zone_entry_defaults` in `__init__.py`); the switch is also the bypass state indicator (published only from the panel's `ZONE_STATE` bypass bitmap, never optimistically). `CrowAlarmPanelZoneBypassSwitch` lives in the parent's C++ files (not `switch/`) because parent-created entities must compile even without a `switch:` platform entry in YAML. In the parent `__init__.py`, core component imports are aliased (`binary_sensor_component`, `switch_component`) because the package's own child platforms shadow the unaliased names once imported.
- `crow_alarm_control_panel.cpp` adapts ESPHome alarm-control-panel calls onto the parent's `arm_away()` / `arm_stay()` / `disarm()`.
- Child platforms (`binary_sensor`, `text_sensor`, `switch`, `button`, `alarm_control_panel`) are thin Python glue: define a schema, resolve the parent via `crow_alarm_panel_id` with `cg.get_variable(...)`, then call a parent registration method implemented in C++.

## Protocol documentation

`components/crow_alarm_panel/docs/` holds the reverse-engineering notes. Consult before touching bus-level C++:

- `protocol_wire_format.md` — frame structure, bit encoding, boundary bytes
- `keypad_protocol_types.md` — packet type catalogue (known vs inferred meanings)
- `arm_disarm_state_machine.md` / `output_select_state_machine.md` — state machine design rationale
- `protocol_investigations.md` — raw observations and open questions

Distinguish observed facts from inferred hypotheses when adding to these docs or to log messages; unknown packet meanings are labeled conservatively. `skills/protocol-reverse-engineering/skill.md` defines the analysis methodology used for trace work.

## Key conventions

- Arm/disarm and output flows are intentionally funneled through the queued keypress state machines in `CrowAlarmPanel` — never send packets directly from buttons or the alarm control panel, or the bus-idle checks, spacing, and in-progress guards are bypassed.
- Features usually need wiring in three places: the Python schema (parent or child), the C++ implementation, and `crow_alarm_panel_test.yaml` if the feature should stay covered by local validation.
- `CrowAlarmPanel::setup()` auto-adds a `"Virtual Keypad"` entry for the configured keypad address if not listed in YAML; don't duplicate that fallback elsewhere.
- Keep `AUTO_LOAD`, the Python package layout, and the parent registration API in sync when adding/removing child platforms. `pyproject.toml` only packages `crow_alarm_panel*` from `components/`, so packaging matters if you add or rename a component.
- All child platform `to_code` functions use `async def` + `await`; don't reintroduce the legacy generator-style `yield` coroutines.
- Python tooling stays under `uv` (commitlint is in the `dev` group); don't introduce Node-based commit tooling.
