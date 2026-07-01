# Crow Alarm Panel — Standard Protocol Comparison

**Purpose:** Document why the Crow keypad bus requires a custom ISR-driven driver and cannot
be handled by any standard ESPHome serial or bus component.

---

## Crow Bus Characteristics (Summary)

| Property | Value |
|---|---|
| Physical wires | 2 — separate CLK and DAT |
| Clock source | External, driven by alarm controller (~1.2 kHz) |
| Data line topology | Open-drain, shared multi-drop (all keypads share one DAT wire) |
| Bus direction | Half-duplex (controller and keypads take turns on the same DAT line) |
| Bit order | LSB-first; sampled on falling edge of external CLK |
| Framing | `0x7E` boundary bytes (HDLC-style, no byte-stuffing observed) |
| Checksum | None observed |
| Addressing | Address byte inside every packet payload |

See `protocol_wire_format.md` for the full physical-layer and frame specification.

---

## Candidate Protocols

### 1-Wire (Dallas/Maxim — ESPHome `one_wire`)

| Dimension | 1-Wire | Crow |
|---|---|---|
| Physical wires | 1 (data + phantom power combined) | 2 (CLK separate from DAT) |
| Clock | Self-timed — bit values encoded in pulse widths (15 µs / 60 µs slots) | External controller drives CLK at ~1.2 kHz |
| Bit encoding | Timing-based (pulse duration determines 0 vs 1) | Level-based (DAT high/low at each CLK falling edge) |
| Protocol stack | ROM commands, family codes, CRC-8 | Proprietary message types, no CRC |
| ESPHome role | Always master (generates reset pulses, initiates slots) | Must participate as a bus node (sniff + transmit) |

**Verdict: Not reusable.**
The physical layer is incompatible at the most fundamental level. 1-Wire has no separate
clock wire and encodes data in pulse timing; Crow requires an external clock and uses
simple level sampling. The two protocols share nothing beyond an open-drain bus topology.

---

### UART (async serial)

| Dimension | UART | Crow |
|---|---|---|
| Clock | None — baud rate is implicit, self-timed | External CLK wire driven by controller |
| Framing | Start bit + 8 data bits + stop bit per byte | `0x7E` boundary bytes per packet |
| Bus topology | Point-to-point | Shared multi-drop open-drain |
| Hardware | SoC UART peripheral | Requires GPIO ISR on clock pin |

**Verdict: Not reusable.**
Crow is synchronous (externally clocked); UART is asynchronous. There is no way to feed
an external 1.2 kHz clock into a UART peripheral. Framing and topology are also
incompatible.

---

### SPI

| Dimension | SPI | Crow |
|---|---|---|
| Wires | 4 (MOSI, MISO, SCK, CS) or 3 | 2 (CLK + shared DAT) |
| Data lines | Full-duplex MOSI/MISO | Half-duplex single shared DAT |
| Framing | None (byte boundaries implied by SCK count and CS) | `0x7E` packet delimiters |
| Chip-select | Required to gate each transaction | Not present — framing is in-band |
| ESPHome support | Master-only | Need bus participant role |

**Verdict: Not reusable.**
SPI requires separate transmit and receive lines and a chip-select pin. Crow uses a
single shared data line with no CS signal. ESPHome SPI is master-only and has no
half-duplex shared-bus mode.

---

### I²C

| Dimension | I²C | Crow |
|---|---|---|
| Physical wires | 2 (SDA + SCL) — superficially identical | 2 (DAT + CLK) |
| Bus topology | Open-drain, multi-drop | Open-drain, multi-drop |
| Clock speed | 100 kHz / 400 kHz / 1 MHz | ~1.2 kHz |
| Clock source | Bus master generates SCL | Controller generates CLK externally |
| Framing | START/STOP conditions (SDA transitions while SCL high) | `0x7E` boundary bytes |
| Addressing | 7-bit or 10-bit address per transaction | Address byte inside packet payload |
| ACK/NAK | 1-bit acknowledgement after every byte | None — controller sends `KEYPAD_COMMAND` packets instead |
| ESPHome support | Master-only | Need bus participant role |

**Verdict: Not reusable.**
Despite the appealing physical similarity, I²C and the Crow bus are incompatible at the
framing layer. The 1.2 kHz clock would cause any I²C peripheral or bit-bang driver to
mis-frame every transaction. ESPHome's I²C component provides no bus-sniffer or
bus-participant mode. The START/STOP condition signalling and per-byte ACK scheme have
no equivalent in the Crow protocol.

---

### HDLC / PPP (framing only)

| Dimension | HDLC | Crow |
|---|---|---|
| Frame delimiter | `0x7E` | `0x7E` — identical |
| Byte-stuffing | `0x7D 0x5E` escapes `0x7E` in payload | Not observed; no stuffing in any captured frame |
| Physical transport | Typically async UART or synchronous HDLC | Synchronous, externally-clocked open-drain bus |
| ESPHome support | No HDLC component exists | — |

**Verdict: Framing coincidence only; not reusable.**
The `0x7E` delimiter is a coincidental match with HDLC. No ESPHome HDLC component
exists and the physical transport is incompatible. One open question remains: if a
payload byte value ever equals `0x7E`, the current parser would misinterpret it as an
end-of-frame boundary. No byte-stuffing has been observed in captures, suggesting
either the protocol reserves `0x7E` from payload values or the observed message types
simply never produce that byte value in practice. This should be tracked as a robustness
concern if additional message types are reverse-engineered.

---

## Overall Conclusion

The Crow keypad bus is a **proprietary synchronous serial multi-drop bus** with no
direct equivalent in the set of standard protocols supported by ESPHome:

- The externally driven ~1.2 kHz clock rules out UART and 1-Wire.
- The half-duplex shared open-drain single-data topology rules out SPI.
- The proprietary `0x7E` packet framing and message schema rule out I²C and HDLC.
- The need to act as a bus *participant* (not just master) rules out all
  ESPHome master-only drivers (I²C, SPI).

**The custom ISR-driven two-wire bit-bang implementation in `CrowAlarmPanelStore` is
the correct and necessary approach.** The clock-pin interrupt captures falling edges at
~1.2 kHz without polling; the open-drain TX path releases/pulls the data line
synchronously with those same clock edges. No standard ESPHome component can
substitute for this.

---

## Open Question: `0x7E` in Payload

The parser uses a sliding 8-bit window to detect `0x7E` boundaries. A payload byte
that equals `0x7E` would prematurely terminate a frame. No such case has been observed
in practice, but it has not been ruled out for unknown message types. If confirmed, a
byte-stuffing or alternative framing scheme would be needed.

---

## Cross-references

| Topic | Document |
|---|---|
| Physical layer and frame format | `protocol_wire_format.md` |
| Observed message types | `protocol_wire_format.md` (Message Types section) |
| Output-select state machine | `output_select_state_machine.md` |
| Arm/disarm state machine | `arm_disarm_state_machine.md` |
| ACK turn-around timing | `protocol_trace_2026-05-12_ack_timing.md` |
