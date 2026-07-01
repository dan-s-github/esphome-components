# Crow Alarm Panel Component


This component allows reading and decoding most messages sent on the `keypad bus` of an Arrowhead Crow Alarm Panel.

It requires 2 wires to the panel, `data` and `clock`. These are usually marked `DAT` and `CLK`.


## Example YAML

This example will just log every message it sees on the keypad bus.

```yaml
crow_alarm_panel:
  clock_pin: REPLACEME
  data_pin: REPLACEME
  address: 8

  on_message:
    - logger.log:
        format: "%02x -  %s"
        args: 
        - "type"
        - "format_hex_pretty(data).c_str()"
```

## Built-in debug logs

With `logger:` set to `DEBUG`, the component also emits decoded runtime logs for observed controller-status packets, keypad polls, output-select acknowledgements, keypad commands/states, and controller time updates. Unknown or inferred packet meanings are still labeled conservatively in the log output.
