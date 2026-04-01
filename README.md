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
2. Create and activate a virtual environment:
   ```bash
   uv sync
   source .venv/bin/activate   # Linux/macOS
   # or
   .venv\Scripts\activate      # Windows
   ```

### Validate / compile a component

A sample test configuration is provided at [`crow_alarm_panel_test.yaml`](crow_alarm_panel_test.yaml). Create a `secrets.yaml` file with your credentials (see ESPHome docs), then run:

```bash
# Validate the configuration
esphome config crow_alarm_panel_test.yaml

# Compile the firmware
esphome compile crow_alarm_panel_test.yaml
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
