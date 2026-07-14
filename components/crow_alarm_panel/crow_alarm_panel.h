#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/alarm_control_panel/alarm_control_panel.h"
#include "esphome/components/alarm_control_panel/alarm_control_panel_state.h"

#include <bitset>
#include <vector>

namespace esphome {
namespace crow_alarm_panel {


static const uint8_t CONTROLLER_STATUS = 0x10;

static const uint8_t ARMED_STATE = 0x11;
static const uint8_t ZONE_STATE = 0x12;
static const uint8_t KEYPAD_COMMAND = 0x14;  // Beeps etc
static const uint8_t KEYPAD_STATE = 0x15;

static const uint8_t SETTING_VALUE = 0x16;   // for locking LEDs
static const uint8_t SETTING_VALUE2 = 0x17;  // for flashing LEDs < 255
static const uint8_t SETTING_VALUE3 = 0x18;  // for flashing LEDs > 255

static const uint8_t BYPASS_STATUS = 0x1B;  // Bypass status; byte 1 = bypass bitmap (sent on BYPASS press and after each zone toggle)

static const uint8_t MEMORY_EVENT = 0x20;

static const uint8_t OUTPUT_SELECT_ACK = 0x1D;

static const uint8_t OUTPUT_STATE = 0x50;
static const uint8_t CURRENT_TIME = 0x54;
static const uint8_t KEYPAD_PING = 0x23;  // Observed recurring keypad keep-alive/ping traffic
static const uint8_t KEYPAD_REGISTRATION = 0xA0;  // Keypad announce: [a0.address.00]; sent on power-up/reset
static const uint8_t BOUNDARY = 0x7E;
// static const uint8_t KEYPRESS = 0xD1; // This is from upstream, but doesn't get sent by arrowhead panels that I can see
static const uint8_t KEYPRESS = 0xA1;
static const uint8_t MEMORY_CLEAR = 0xD2;

// Keys 0 - 9 are 0-9
static const uint8_t KEY_OUTPUT = 0x0A;   // 10
static const uint8_t KEY_MEMORY = 0x0B;   // 11
static const uint8_t KEY_ARM = 0x0D;      // 13
static const uint8_t KEY_STAY = 0x0E;     // 14, untested
static const uint8_t KEY_BYPASS = 0x0F;   // 15
static const uint8_t KEY_PROGRAM = 0x10;  // 16
static const uint8_t KEY_ENTER = 0x11;    // 17

static const uint8_t BUFFER_LENGTH = 20;

static const char *KEYS[18] = {"0", "1",     "2",      "3",       "4",   "5",    "6",      "7",       "8",
                               "9", "OUTPUT", "MEMORY", "CONTROL", "ARM", "STAY", "BYPASS", "PROGRAM", "ENTER"};

static const uint8_t RESPONSE_TIME = 0x19;

static const char *DAYS[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

enum class OutputSelectState : uint8_t {
  IDLE,
  OUTPUT_PENDING,  // sent OUTPUT keypress, waiting for 0x1D ACK addressed to us
  AWAIT_COMMAND,   // got 0x1D, waiting for first 0x14 KEYPAD_COMMAND
  DIGIT_PENDING,   // sent a digit, waiting for 0x14 KEYPAD_COMMAND confirmation
  ENTER_DELAY,     // all digits confirmed; waiting ~60 ms before sending ENTER so the
                   // controller's output-fire broadcast clears the bus first
  ENTER_PENDING,   // sent ENTER, waiting for final 0x14 KEYPAD_COMMAND confirmation
};

enum class ArmDisarmState : uint8_t {
  IDLE,
  ARM_AWAY_PENDING,   // sent KEY_ARM (no code), waiting for KEYPAD_COMMAND
  ARM_STAY_PENDING,   // sent KEY_STAY (no code), waiting for KEYPAD_COMMAND
  CODE_DIGIT_PENDING, // sent a code digit (arm-with-code or disarm), waiting for KEYPAD_COMMAND
  // Sent terminal key (KEY_ARM/KEY_STAY/KEY_ENTER). KEYPAD_COMMAND byte[1] here is NOT used to
  // decide success/failure — logs-10/35 and logs-12/37 (2026-07-12) show it's unreliable in both
  // directions: a 0x01 ack can precede a sequence that succeeds moments later, AND a fully clean
  // run of non-0x01 acks (no ambiguity at all) can still silently not disarm the panel. Any
  // KEYPAD_COMMAND received here just proves the controller is alive; the sequence only resolves
  // to IDLE via the independent ARMED_STATE broadcast (ground truth) or the shared 1s watchdog
  // timing out (treated as failure). See arm_disarm_state_machine.md.
  CODE_ENTER_PENDING,
};

// BYPASS → zone digit(s) → ENTER. TRACE-VERIFIED (see docs/zone_bypass_state_machine.md):
// unlike the arm/disarm code sequence, the digits and ENTER are sent back-to-back after the
// first KEYPAD_COMMAND (0x14) rather than waiting for a confirmation per key.
enum class ZoneBypassState : uint8_t {
  IDLE,
  BYPASS_PENDING,  // sent KEY_BYPASS + first digit, waiting for KEYPAD_COMMAND
  ENTER_PENDING,   // sent remaining digit + ENTER, waiting for final KEYPAD_COMMAND
};

struct CrowAlarmPanelMessage {
  uint8_t type;
  uint8_t data_buffer[BUFFER_LENGTH];
  uint8_t data_len;
};

struct CrowAlarmPanelKeypad {
  std::string name;
  uint8_t address;
};

// static const CrowAlarmPanelKeypad UNKNOWN_KEYPAD{
//   .address = 0xFF,
//   .name = "Unknown"
// };

class CrowAlarmPanelStore {
 public:
  void setup(InternalGPIOPin *clock_pin, InternalGPIOPin *data_pin);
  static void interrupt(CrowAlarmPanelStore *arg);

  uint8_t buffer[BUFFER_LENGTH];
  uint8_t buffer2[BUFFER_LENGTH];
  uint8_t data_length{0};

  bool data{false};
  uint32_t last_clock_time_{0};             // Track last clock edge
  uint32_t last_transmission_time_{0};      // Track last TX completion
  uint32_t prev_falling_edge_time_us_{0};   // Track last falling edge for glitch filtering

  bool inside_{false};

 protected:
  ISRInternalGPIOPin clock_pin_;
  ISRInternalGPIOPin data_pin_;
  uint8_t num_bits_{0};
  uint8_t boundary_buffer_{0};

 public:
  // Hardware ACK: set from CrowAlarmPanel::setup() before interrupts attach.
  // ack_pending_ is set in the ISR when a frame addressed to us ends, and cleared on the next
  // falling edge after driving DAT low for one clock cycle (~1 bus clock, ~416–833µs).
  uint8_t ack_keypad_address_{0xFF};  // set from setup(); 0xFF = never ACK (passive mode)
  volatile bool ack_pending_{false};
  // Timestamp (micros) when ack_pending_ was set. Used by loop() to release the ACK pin
  // if the next clock edge never arrives (e.g. clock stops between packets).
  volatile uint32_t ack_set_time_us_{0};
  // Set true during send_packet_blocking_() so the ISR on the other core
  // ignores clock edges caused by our own transmission.
  volatile bool is_transmitting_{false};
  // Set by ISR while is_transmitting_ is true; cleared on the first edge after
  // transmission ends so the ISR can reset its mid-packet receive state cleanly.
  volatile bool was_transmitting_{false};

  static const uint32_t BUS_IDLE_TIMEOUT_US = 180;           // Allow takeover between bit bursts
  static const uint32_t MIN_TX_INTERVAL_MS = 5;              // Allow keypad-like key burst cadence (conservative)
  /**
  * Minimum interval between falling edges to avoid glitches. The clock runs at ~1.2kHz, so we
  * expect ~833us between edges. Keep this comfortably below that to filter bounce without
  * dropping real edges.
  */
  static const uint32_t MIN_FALLING_EDGE_INTERVAL_US = 700;  // Filter bounce without dropping real edges
  static const uint32_t TX_START_TIMEOUT_US = 100000;        // 100ms to start TX
  static const uint32_t TX_BIT_TIMEOUT_US = 10000;           // 10ms between bits

  // Delay between digit confirmation and sending ENTER in the output-select sequence.
  // Real keypads wait for the user to press ENTER (~100 ms after digit confirmation).
  // Without this delay our ENTER TX overlaps the controller's KEYPAD_COMMAND [15]
  // (exit output-select) reply, causing the ISR's was_transmitting_ reset to discard it.
  // The controller then keeps the keypad stuck in "output-select mode", rejecting all
  // subsequent KEY_OUTPUT attempts with KEYPAD_COMMAND [07].
  static const uint32_t OUTPUT_SELECT_ENTER_DELAY_MS = 60;
};

struct CrowAlarmPanelZone {
  binary_sensor::BinarySensor *motion_binary_sensor;
  switch_::Switch *bypass_switch;  // bypass state indicator AND control (no separate sensor)
  uint8_t zone;
};

struct CrowAlarmPanelOutput {
  switch_::Switch *the_switch;
  uint8_t number;
};

class CrowAlarmPanel : public Component {
 public:
  void setup() override;
  void dump_config() override;
  void loop() override;

  void set_output(uint8_t output, bool state);
  void set_zone_bypass(uint8_t zone, bool state);

  void set_clock_pin(InternalGPIOPin *clock) { this->clock_pin_ = clock; }
  void set_data_pin(InternalGPIOPin *data) { this->data_pin_ = data; }
  void set_keypad_address(uint8_t address) { this->keypad_address_ = address; }
  bool is_active_keypad() const { return this->keypad_address_ <= 7; }
  // Panel-level alarm code; entities without their own code fall back to this.
  void set_code(const std::string &code) { this->code_ = code; }
  const std::string &get_code() const { return this->code_; }
  void add_keypad(const std::string &name, uint8_t address) {
    this->keypads_.push_back(std::move(CrowAlarmPanelKeypad{
        .name = name,
        .address = address,
    }));
  }

  void register_zone(binary_sensor::BinarySensor *binary_sensor, uint8_t zone) {
    for (auto &z : this->zones_) {
      if (z.zone == zone) {
        z.motion_binary_sensor = binary_sensor;
        return;
      }
    }
    this->zones_.push_back(CrowAlarmPanelZone{
        .motion_binary_sensor = binary_sensor,
        .bypass_switch = nullptr,
        .zone = zone,
    });
  }
  void register_zone_bypass_switch(switch_::Switch *bypass_switch, uint8_t zone) {
    for (auto &z : this->zones_) {
      if (z.zone == zone) {
        z.bypass_switch = bypass_switch;
        return;
      }
    }
    this->zones_.push_back(CrowAlarmPanelZone{
        .motion_binary_sensor = nullptr,
        .bypass_switch = bypass_switch,
        .zone = zone,
    });
  }

  void register_armed_state(text_sensor::TextSensor *armed_state_sensor) { this->armed_state_ = armed_state_sensor; }
  void register_output_switch(switch_::Switch *output_switch, uint8_t output_number) {
    this->outputs_.push_back(std::move(CrowAlarmPanelOutput{
        .the_switch = output_switch,
        .number = output_number,
    }));
  }
  void register_alarm_control_panel(alarm_control_panel::AlarmControlPanel *acp) { this->alarm_control_panel_ = acp; }

  void arm_away(const std::string &code = "");
  void arm_stay(const std::string &code = "");
  void disarm(const std::string &code);
  bool is_armed() const;

  Trigger<uint8_t, std::vector<uint8_t>> *get_on_message_trigger() const { return this->on_message_trigger_; }

  void send_packet(uint8_t type, const std::vector<uint8_t> &data);

  void keypress(uint8_t key);

 protected:
  CrowAlarmPanelKeypad find_keypad_(uint8_t address);
  bool is_bus_idle_();
  void start_code_sequence_(const std::string &code, uint8_t terminal_key);

  void send_packet_blocking_(const std::vector<uint8_t> &packet);
  bool wait_for_clock_edge_(bool wait_for_state, uint32_t timeout_us);

  // One-shot registration announce sent after boot delay.
  bool registration_sent_{false};
  uint32_t registration_after_ms_{15000};

  // Watchdog: track last time the controller polled us; re-register if silent for 60 s.
  uint32_t last_ping_ms_{0};

  // Output-select state machine.
  OutputSelectState output_select_state_{OutputSelectState::IDLE};
  uint32_t output_select_state_enter_ms_{0};
  std::vector<uint8_t> output_select_keys_;  // digit(s) to send, consumed one per KEYPAD_COMMAND
  uint8_t output_select_key_idx_{0};
  uint8_t output_select_retry_count_{0};  // max 1 automatic retry before aborting

  // Zone-bypass state machine. The panel toggles bypass per BYPASS/digits/ENTER sequence,
  // so the switch state is only published from ZONE_STATE bypass-bitmap broadcasts, never
  // optimistically.
  ZoneBypassState zone_bypass_state_{ZoneBypassState::IDLE};
  uint32_t zone_bypass_state_enter_ms_{0};
  std::vector<uint8_t> zone_bypass_keys_;
  uint8_t zone_bypass_key_idx_{0};
  uint8_t zone_bypass_target_zone_{0};   // for logging only
  uint8_t zone_bypass_initial_bitmap_{0};  // BB from the first KEYPAD_COMMAND after BYPASS

  // Arm/disarm state machine.
  ArmDisarmState arm_disarm_state_{ArmDisarmState::IDLE};
  uint32_t arm_disarm_state_enter_ms_{0};
  std::vector<uint8_t> arm_disarm_code_digits_;  // code digits consumed one per KEYPAD_COMMAND
  uint8_t arm_disarm_code_idx_{0};
  uint8_t arm_disarm_terminal_key_{KEY_ENTER};  // KEY_ENTER (disarm), KEY_ARM or KEY_STAY (arm-with-code)
  // "Digit accepted" display_code (KEYPAD_COMMAND byte[1]) learned from the first digit's
  // response each sequence. Physical keypad types disagree on this value (0x01 is common,
  // but address 0x05 has been observed sending 0x07 for the same "more digits expected"
  // semantic — see docs/arm_disarm_state_machine.md), so it can't be hardcoded to 0x01.
  uint8_t arm_disarm_digit_ack_byte_{0};
  bool arm_disarm_digit_ack_byte_set_{false};

  CrowAlarmPanelStore store_;
  InternalGPIOPin *clock_pin_{nullptr};
  InternalGPIOPin *data_pin_{nullptr};
  uint8_t keypad_address_{0xFF};  // 0xFF = not configured (passive monitor mode)
  std::string code_;
  text_sensor::TextSensor *armed_state_{nullptr};
  alarm_control_panel::AlarmControlPanel *alarm_control_panel_{nullptr};
  Trigger<uint8_t, std::vector<uint8_t>> *on_message_trigger_{new Trigger<uint8_t, std::vector<uint8_t>>()};

  std::vector<CrowAlarmPanelZone> zones_;
  std::vector<CrowAlarmPanelKeypad> keypads_;
  std::vector<CrowAlarmPanelOutput> outputs_;
  // Longest configured keypad label, computed once in setup(); pads the "[label]" prefix in
  // log messages so the text after it lines up regardless of which keypad is logging.
  uint8_t keypad_label_width_{0};
};

// Bypass toggle switch created by the parent's `zones:` config. Lives here (not in switch/)
// because zone entities are instantiated from the parent component, which must compile even
// when no `switch:` platform entry exists in YAML.
class CrowAlarmPanelZoneBypassSwitch : public switch_::Switch, public Component {
 public:
  void dump_config() override;

  void set_parent(CrowAlarmPanel *parent) { this->parent_ = parent; }
  void set_zone_number(uint8_t zone_number) { this->zone_number_ = zone_number; }

 protected:
  void write_state(bool state) override;

  CrowAlarmPanel *parent_{nullptr};
  uint8_t zone_number_{0};
};

class CrowAlarmControlPanel : public alarm_control_panel::AlarmControlPanel, public Component {
 public:
  void dump_config() override;
  uint32_t get_supported_features() const override {
    return alarm_control_panel::ACP_FEAT_ARM_AWAY | alarm_control_panel::ACP_FEAT_ARM_HOME;
  }
  // No default code configured means the user must supply one when disarming.
  bool get_requires_code() const override { return this->effective_code_().empty(); }
  bool get_requires_code_to_arm() const override { return this->requires_code_to_arm_; }

  void set_parent(CrowAlarmPanel *parent) { this->parent_ = parent; }
  void set_code(const std::string &code) { this->code_ = code; }
  void set_requires_code_to_arm(bool requires_code_to_arm) { this->requires_code_to_arm_ = requires_code_to_arm; }

 protected:
  void control(const alarm_control_panel::AlarmControlPanelCall &call) override;
  // Entity-level code if set, else the parent panel's code (or empty if parent_ isn't set yet).
  const std::string &effective_code_() const {
    if (!this->code_.empty()) {
      return this->code_;
    }
    static const std::string empty_code;
    return this->parent_ != nullptr ? this->parent_->get_code() : empty_code;
  }
  // The API always sends a code field (empty string when the user didn't enter one), so an empty
  // call code must fall back to the configured default rather than shadow it.
  std::string resolve_code_(const alarm_control_panel::AlarmControlPanelCall &call) const {
    const auto &call_code = call.get_code();
    if (call_code.has_value() && !call_code->empty()) {
      return call_code.value();
    }
    return this->effective_code_();
  }

  CrowAlarmPanel *parent_{nullptr};
  std::string code_;
  bool requires_code_to_arm_{false};
};

}  // namespace crow_alarm_panel
}  // namespace esphome
