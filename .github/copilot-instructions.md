# Copilot Instructions

## Build, test, and validation commands

Local development uses `uv` and the repo is intended to run on Python 3.13 for local work (`pyproject.toml` allows 3.11-3.13, but the README notes 3.14+ is not supported because PlatformIO wheels are missing).

| Task | Command |
| --- | --- |
| Create/update the local environment | `uv sync` |
| Lint an explicit commit message | `uv run commitlint "feat: add keypad monitor fixture"` |
| Lint the latest commit message | `uv run commitlint --hash HEAD` |
| Lint a commit range | `uv run commitlint --from-hash <base> --to-hash HEAD` |
| Install the commit-msg hook | `uv run pre-commit install --hook-type commit-msg` |
| Validate the canonical integration fixture | `uv run esphome config crow_alarm_panel_test.yaml` |
| Compile the canonical integration fixture | `uv run esphome compile crow_alarm_panel_test.yaml` |
| Validate a single fixture while iterating | `uv run esphome config path/to/your_fixture.yaml` |
| Compile a single fixture while iterating | `uv run esphome compile path/to/your_fixture.yaml` |

There is no separate source-code lint target or Python unit-test suite in the current repository; ESPHome config validation/compile and commit-message linting are the practical checks.

## High-level architecture

This repository is an ESPHome external components repo. The Python package metadata in `pyproject.toml` points setuptools at `components/`, and only `crow_alarm_panel*` is currently included as a package, so packaging changes matter if you add or rename components.

`components/crow_alarm_panel/__init__.py` is the top-level ESPHome integration entrypoint. It defines the parent `crow_alarm_panel:` YAML schema, declares the `CrowAlarmPanel` and `CrowAlarmControlPanel` C++ classes, auto-loads the child platforms (`binary_sensor`, `text_sensor`, `switch`, `button`, `alarm_control_panel`), and wires the optional `on_message` automation trigger.

The runtime behavior lives mostly in C++:

- `crow_alarm_panel.h` / `crow_alarm_panel.cpp` implement the bus protocol handler. `CrowAlarmPanelStore` captures keypad bus traffic from GPIO interrupts, while `CrowAlarmPanel::loop()` parses completed frames and publishes state updates to registered ESPHome entities.
- Outbound actions also go through `CrowAlarmPanel`. Keypresses and packet sends are serialized there so transmissions only happen when the bus is idle and timing guards are satisfied.
- `crow_alarm_control_panel.cpp` adapts ESPHome alarm-control-panel calls into parent `arm_away()`, `arm_stay()`, and `disarm()` operations.

Each child platform is thin Python glue that looks up a parent instance via `crow_alarm_panel_id` and registers itself back onto that parent:

- `binary_sensor/__init__.py` registers zone and bypass sensors
- `text_sensor/__init__.py` registers the armed-state text sensor
- `switch/__init__.py` registers output switches
- `button/__init__.py` registers arm/disarm helper buttons
- `alarm_control_panel/__init__.py` registers the higher-level alarm control panel entity

`crow_alarm_panel_test.yaml` is the canonical end-to-end fixture for local validation. It uses `external_components` with `type: local` and `path: components`, so it exercises the repo the same way ESPHome users consume it during development.

## Protocol documentation

`components/crow_alarm_panel/docs/` contains detailed protocol reverse-engineering notes. Before modifying bus-level C++ code, consult:

- `protocol_wire_format.md` — frame structure, bit encoding, boundary bytes
- `arm_disarm_state_machine.md` / `output_select_state_machine.md` — state machine design rationale
- `keypad_protocol_types.md` — packet type catalogue with known/inferred meanings
- `protocol_investigations.md` — raw observations and open questions

`traces/` holds CSV and raw captures used during reverse-engineering. Cross-reference these when interpreting ambiguous packet types.

## Key conventions

- Treat the Python files under `components/crow_alarm_panel/**/__init__.py` as schema/codegen glue, not the place for protocol logic. Bus parsing, timing, queueing, and state transitions belong in the C++ implementation.
- Child entities are always attached to a specific parent `crow_alarm_panel` instance via `crow_alarm_panel_id`. When adding a new child platform, follow the existing pattern: define the schema in Python, resolve the parent with `cg.get_variable(...)`, then call a parent registration method implemented in C++.
- Features usually need wiring in three places, not one: the top-level or child Python schema, the C++ parent/entity implementation, and the sample YAML fixture if the feature should remain covered by local validation.
- Arm/disarm flows are intentionally funneled through queued keypress logic in `CrowAlarmPanel` instead of sending packets directly from buttons or the alarm control panel. Preserve that pattern so bus-idle checks, spacing, and in-progress guards continue to work.
- `CrowAlarmPanel::setup()` auto-adds a `"Virtual Keypad"` entry for the configured keypad address if it was not listed in YAML. Do not duplicate that behavior elsewhere; rely on the parent setup path for keypad-name fallback.
- `AUTO_LOAD` and `MULTI_CONF = True` in the parent module are part of the integration shape. If you add or remove child platforms, keep `AUTO_LOAD`, the Python package layout, and the parent registration API in sync.
- Child platform `to_code` functions are inconsistently styled: some use `async def` + `await` (`button`, `alarm_control_panel`), others use generator-style `yield` (`binary_sensor`, `text_sensor`, `switch`). New platforms should use `async def` + `await`; do not mix both styles in a single function.
- This repository keeps Python tooling under `uv`, including `commitlint` in the `dev` dependency group. Prefer `uv run commitlint ...` instead of introducing separate Node-based commit-message tooling.
- The repo includes a local `.pre-commit-config.yaml` commit-msg hook that shells out to `uv run commitlint --file`. If commit-message linting should run automatically, install it with `uv run pre-commit install --hook-type commit-msg`.
