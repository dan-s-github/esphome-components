# esphome-components

Custom ESPHome components. The `components/` directory contains components that can be used with the ESPHome `external_components` feature.

## Components

| Component | Description |
|-----------|-------------|
| [crow_alarm_panel](components/crow_alarm_panel/README.md) | Integration for Arrowhead Crow alarm panels via the keypad bus |

## Local development with uv

[uv](https://docs.astral.sh/uv/) is used to manage the Python build environment so that components can be compiled and tested locally.

### Setup

1. Install uv: https://docs.astral.sh/uv/getting-started/installation/
2. Run `uv sync` — this creates a `.venv` using Python 3.13 (3.14+ is not supported due to missing binary wheels for PlatformIO dependencies)

### Commit message linting

`commitlint` is included in the `dev` dependency group, so after `uv sync` you can lint commit messages with:

```bash
# Lint an explicit message
uv run commitlint "feat: add keypad monitor fixture"

# Lint the latest commit
uv run commitlint --hash HEAD
```

To enforce this automatically when creating commits, install the `commit-msg` hook:

```bash
uv run pre-commit install --hook-type commit-msg
```

### Validate / compile a component

A sample test configuration is provided at [`crow_alarm_panel_test.yaml`](crow_alarm_panel_test.yaml). Create a `secrets.yaml` file with your credentials (see ESPHome docs), then run:

```bash
# Validate the configuration
uv run esphome config crow_alarm_panel_test.yaml

# Compile the firmware
uv run esphome compile crow_alarm_panel_test.yaml
```

### Using components in your own ESPHome configuration

Reference the `components/` directory via `external_components`:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/dan-s-github/esphome-components
      ref: main
    components: [crow_alarm_panel]
```
