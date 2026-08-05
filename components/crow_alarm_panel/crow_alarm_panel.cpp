#include "crow_alarm_panel.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/gpio.h"

#define HIGH 1
#define LOW 0

namespace esphome {
namespace crow_alarm_panel {

static const char *TAG = "crow_alarm_panel";
// Log prefix for panel-wide broadcasts that carry no keypad address (OUTPUT_STATE, ZONE_STATE,
// ARMED_STATE, CURRENT_TIME, RESPONSE_TIME, SETTING_VALUE*, MEMORY_EVENT, unknown types).
static const char *CONTROLLER_LABEL = "Controller";

void CrowAlarmPanelStore::setup(InternalGPIOPin *clock_pin, InternalGPIOPin *data_pin) {
  clock_pin->setup();
  data_pin->setup();
  this->clock_pin_ = clock_pin->to_isr();
  this->data_pin_ = data_pin->to_isr();
  memset(this->buffer, 0, sizeof(this->buffer));
  memset(this->buffer2, 0, sizeof(this->buffer2));
  this->data_length = 0;
  this->num_bits_ = 0;
  this->boundary_buffer_ = 0;
  clock_pin->attach_interrupt(CrowAlarmPanelStore::interrupt, this, gpio::INTERRUPT_FALLING_EDGE);
}

std::string binary_indices(uint8_t byte) {
  std::string str;
  for (uint8_t i = 0; i < 8; i++) {
    if ((byte >> i) & 0x01) {
      if (str.length() > 0) {
        str += ",";
      }
      str += to_string(i + 1);
    }
  }
  return str;
}

std::string keypad_label(const CrowAlarmPanelKeypad &keypad, uint8_t address) {
  if (!keypad.name.empty()) {
    return keypad.name;
  }
  return str_sprintf("Keypad 0x%02X", address);
}

// Sakamoto's algorithm. Returns the protocol's day_of_week encoding directly (1=Sunday..7=Saturday,
// matching DAYS[] and CURRENT_TIME's data[0]) rather than the usual 0=Sunday.
uint8_t day_of_week_from_date(uint16_t year, uint8_t month, uint8_t day) {
  static const uint8_t OFFSETS[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 3) {
    year--;
  }
  return static_cast<uint8_t>((year + year / 4 - year / 100 + year / 400 + OFFSETS[month - 1] + day) % 7) + 1;
}

const char *controller_status_profile(uint8_t flags) {
  if (flags == 0x80) {
    return "zone_activity";
  }
  if (flags == 0xC1) {
    return "zones_clear";
  }
  if (flags & 0x80) {
    return "zone_activity?";
  }
  if ((flags & 0xC1) == 0xC1) {
    return "zones_clear?";
  }
  return "unclassified";
}

const char *controller_status_state(uint8_t flags) {
  if (flags == 0x80) {
    return "zone_active_or_transition";
  }
  if (flags == 0xC1) {
    return "zones_clear";
  }
  if (flags & 0x80) {
    return "zone_related?";
  }
  if ((flags & 0xC1) == 0xC1) {
    return "zones_clear?";
  }
  return "unknown";
}

void IRAM_ATTR HOT CrowAlarmPanelStore::interrupt(CrowAlarmPanelStore *arg) {
  // Release hardware ACK on the falling edge immediately after we drove DAT low.
  // This produces ~1 clock cycle (~416–833µs) of DAT-low — matching real keypad ACK behaviour.
  //
  // Deliberately does NOT return here: this same edge can carry the first bit of an immediate
  // controller retransmission. Trace: logs-11/36 (2026-07-12) show the controller resending the
  // same KEYPAD_COMMAND to us ~10x within ~1s (it isn't seeing our ACK land), and our own
  // receiver decoding that burst as garbage (`Unknown [ff.]`/`[fe.]`) while a passive monitor on
  // the same bus decodes it cleanly. Previously this edge was discarded outright — the bit it
  // carried never reached boundary_buffer_/num_bits_ below, silently shifting alignment for
  // whatever frame followed by one bit (the same class of corruption documented for the
  // CURRENT_TIME glitch in protocol_investigations.md, but compounding across a whole retry
  // burst instead of one field). Falling through lets this edge's bit feed the same
  // glitch-filter/boundary-detection path as any other edge instead of leaving a hole in it.
  if (arg->ack_pending_) {
    arg->data_pin_.pin_mode(gpio::FLAG_INPUT);
    arg->ack_pending_ = false;
  }

  // On dual-core ESP32-S3, InterruptLock is core-local: the ISR runs on core 0
  // while loop() runs on core 1. Skip processing during our own transmission so
  // we don't capture our own bits, set ack_pending_, and corrupt the KEYPAD_COMMAND
  // that immediately follows.
  if (arg->is_transmitting_) {
    arg->was_transmitting_ = true;
    return;
  }

  // First edge after transmission ended: ISR was blind for ~33ms so receive state
  // (inside_, num_bits_, buffer, boundary_buffer_) may be mid-packet garbage.
  // Reset everything so we start clean from the next packet boundary.
  if (arg->was_transmitting_) {
    arg->was_transmitting_ = false;
    arg->inside_ = false;
    arg->num_bits_ = 0;
    arg->boundary_buffer_ = 0;
    arg->data = false;
    memset(arg->buffer, 0, BUFFER_LENGTH);
    return;
  }

  uint32_t now = micros();
  arg->last_clock_time_ = now;  // Track last clock edge for bus idle detection

  // Clock glitch filtering - ignore edges that are too close together
  if (now - arg->prev_falling_edge_time_us_ < MIN_FALLING_EDGE_INTERVAL_US) {
    return;  // Glitch detected, ignore this edge
  }
  arg->prev_falling_edge_time_us_ = now;

  // Handle receive mode
  bool data_bit = arg->data_pin_.digital_read();

  if (!arg->data && data_bit)
    return;

  arg->data = true;

  // Check for boundary
  arg->boundary_buffer_ = (uint8_t) ((arg->boundary_buffer_ << 1) | data_bit);

  if (arg->bit_trace_enabled_) {
    arg->bit_trace_buffer_[arg->bit_trace_len_++] = data_bit ? '1' : '0';
    if (arg->bit_trace_len_ >= BIT_TRACE_BUFFER_BITS) {
      memcpy(arg->bit_trace_buffer2_, arg->bit_trace_buffer_, BIT_TRACE_BUFFER_BITS);
      arg->bit_trace_buffer2_[BIT_TRACE_BUFFER_BITS] = '\0';
      arg->bit_trace_len_ = 0;
      arg->bit_trace_ready_ = true;
    }
  }

  if (arg->inside_) {
    uint8_t idx = arg->num_bits_ / 8;
    arg->buffer[idx] = (arg->buffer[idx] >> 1) | ((data_bit ? 1 : 0) << 7);
    arg->num_bits_++;

    if (arg->boundary_buffer_ == BOUNDARY) {
      //  Save data
      memcpy(arg->buffer2, arg->buffer, arg->num_bits_ / 8);
      arg->data_length = arg->num_bits_ / 8;
      //  Reset
      memset(arg->buffer, 0, BUFFER_LENGTH);
      arg->boundary_buffer_ = 0;
      arg->inside_ = false;
      arg->num_bits_ = 0;
      arg->data = false;
      // Hardware ACK: drive DAT low for one clock cycle only for per-keypad frame types
      // addressed to us. Broadcast frame payload bytes must never be treated as an address.
      // buffer2[0]=type, buffer2[1]=addr (for addressed types), buffer2[data_length-1]=0x7E.
      if (arg->data_length >= 3) {
        const uint8_t type = arg->buffer2[0];
        const bool addressed_type = (type == KEYPAD_COMMAND || type == KEYPAD_STATE || type == OUTPUT_SELECT_ACK ||
                                     type == KEYPAD_PING || type == BYPASS_STATUS);
        if (addressed_type && arg->buffer2[1] == arg->ack_keypad_address_) {
          arg->data_pin_.pin_mode(gpio::FLAG_OUTPUT);
          arg->data_pin_.digital_write(0);
          arg->ack_set_time_us_ = now;
          arg->ack_pending_ = true;
        }
      }
      return;
    } else if (arg->num_bits_ >= BUFFER_LENGTH * 8) {
      // Wrong side of boundary.
      arg->inside_ = false;
      arg->num_bits_ = 0;
      memset(arg->buffer, 0, BUFFER_LENGTH);
      arg->boundary_buffer_ = 0;
      arg->data = false;  // clear data flag on overflow
    }
  }

  if (arg->boundary_buffer_ == BOUNDARY) {
    arg->inside_ = true;
    return;
  }
}

void CrowAlarmPanel::setup() {
  // In passive monitor mode (no address configured) the ISR must not ACK any packet.
  this->store_.ack_keypad_address_ = this->keypad_address_;  // 0xFF when not configured
  this->store_.setup(this->clock_pin_, this->data_pin_);

  if (this->is_active_keypad()) {
    // Ensure our configured keypad address is present for logging/lookup
    bool have_self = false;
    for (const auto &kp : this->keypads_) {
      if (kp.address == this->keypad_address_) {
        have_self = true;
        break;
      }
    }
    if (!have_self) {
      this->keypads_.push_back(CrowAlarmPanelKeypad{
          .name = "Virtual Keypad",
          .address = this->keypad_address_,
      });
    }
  }

  // Widest configured name, or the "Keypad 0xNN" fallback used for unrecognized addresses,
  // whichever is longer — keeps log text aligned after the "[label]" prefix either way.
  // "Keypad 0xFF" (11 chars) is also always >= "Controller" (10 chars), so that fallback
  // covers the CONTROLLER_LABEL width too without a separate comparison.
  this->keypad_label_width_ = strlen("Keypad 0xFF");
  for (const auto &kp : this->keypads_) {
    if (kp.name.size() > this->keypad_label_width_) {
      this->keypad_label_width_ = kp.name.size();
    }
  }

  if (this->armed_state_ != nullptr) {
    this->armed_state_->publish_state("disarmed");
  }
  if (this->alarm_control_panel_ != nullptr) {
    this->alarm_control_panel_->publish_state(alarm_control_panel::ACP_STATE_DISARMED);
  }
};

void CrowAlarmPanel::dump_config() {
  ESP_LOGCONFIG(TAG, "Crow Alarm Panel:");
  LOG_PIN("  Clock Pin: ", this->clock_pin_);
  LOG_PIN("  Data Pin: ", this->data_pin_);
}

CrowAlarmPanelKeypad CrowAlarmPanel::find_keypad_(uint8_t address) {
  for (CrowAlarmPanelKeypad keypad : this->keypads_) {
    if (keypad.address == address) {
      return keypad;
    }
  }
  return {};
}

void CrowAlarmPanel::loop() {
  // Safety valve: if ack_pending_ has been set for longer than 2 clock cycles (~2ms)
  // the clock must have stopped between packets. Force-release DAT so we don't hold
  // the bus LOW indefinitely. The ISR's normal edge-triggered release remains the fast path.
  if (this->store_.ack_pending_ && (micros() - this->store_.ack_set_time_us_ > 2000)) {
    this->data_pin_->pin_mode(gpio::FLAG_INPUT);
    this->store_.ack_pending_ = false;
  }

  if (this->store_.bit_trace_ready_) {
    char local_bits[CrowAlarmPanelStore::BIT_TRACE_BUFFER_BITS + 1];
    {
      InterruptLock lock;
      memcpy(local_bits, this->store_.bit_trace_buffer2_, sizeof(local_bits));
      this->store_.bit_trace_ready_ = false;
    }
    ESP_LOGI(TAG, "Raw bit trace: %s", local_bits);
  }

  if (this->store_.data_length) {
    if (this->store_.data_length < 2) {
      ESP_LOGW(TAG, "Discarding short frame (%d bytes)", this->store_.data_length);
      InterruptLock lock;
      memset(this->store_.buffer2, 0, BUFFER_LENGTH);
      this->store_.data_length = 0;
      return;
    }

    uint8_t type;
    std::vector<uint8_t> data;
    {
      InterruptLock lock;
      type = this->store_.buffer2[0];
      data.insert(data.begin(), this->store_.buffer2 + 1, this->store_.buffer2 + this->store_.data_length - 1);
      memset(this->store_.buffer2, 0, BUFFER_LENGTH);
      this->store_.data_length = 0;
    }

    if (this->raw_frame_logging_enabled_) {
      ESP_LOGI(TAG, "Received raw frame [%02x.%s]", type, format_hex_pretty(data).c_str());
    } else {
      ESP_LOGV(TAG, "Received raw frame [%02x.%s]", type, format_hex_pretty(data).c_str());
    }

    switch (type) {
      case CONTROLLER_STATUS: {
        if (data.size() < 5) {
          ESP_LOGW(TAG, "Controller status too short, discarding");
          break;
        }
        CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        std::string bits = binary_indices(data[2]);
        if (bits.empty()) {
          bits = "none";
        }
        ESP_LOGD(TAG,
                 "[%-*s] Controller status: b1=0x%02X flags=0x%02X bits=%s b3=0x%02X b4=0x%02X profile:%s state:%s "
                 "[%02x.%s]",
                 this->keypad_label_width_, keypad_label(keypad, data[0]).c_str(), data[1], data[2], bits.c_str(),
                 data[3], data[4],
                 controller_status_profile(data[2]), controller_status_state(data[2]), type,
                 format_hex_pretty(data).c_str());
        break;
      }
      case OUTPUT_STATE:
        if (data.size() < 1) {
          ESP_LOGW(TAG, "[%-*s] Output state too short, discarding", this->keypad_label_width_, CONTROLLER_LABEL);
          break;
        }
        ESP_LOGD(TAG, "[%-*s] Output state [%s]", this->keypad_label_width_, CONTROLLER_LABEL,
                 format_hex_pretty(data).c_str());
        for (CrowAlarmPanelOutput output : this->outputs_) {
          bool on = ((data[0] >> (output.number - 1)) & 0x01);
          output.the_switch->publish_state(on);
        }
        break;
      case ZONE_STATE: {
        if (data.size() < 6) {
          ESP_LOGW(TAG, "[%-*s] Zone state invalid length, discarding", this->keypad_label_width_, CONTROLLER_LABEL);
          break;
        }
        // broadcast_type: 0x00 = incremental, 0x01 = full broadcast (sent after registration).
        // Full broadcasts carry accurate active/alarmed bitmaps but always have bypassed=0x00,
        // even when zones are actually bypassed. Skip bypass updates for full broadcasts.
        const bool is_full_broadcast = (data[0] == 0x01);
        ESP_LOGD(TAG, "[%-*s] Zone state received [%s]%s", this->keypad_label_width_, CONTROLLER_LABEL,
                 format_hex_pretty(data).c_str(), is_full_broadcast ? " (full broadcast, bypass skipped)" : "");
        // Walk every zone slot the bitmap can represent (2 banks x 8 bits), not just zones
        // declared under `zones:` in YAML. Otherwise activity on an unconfigured zone is
        // silently swallowed and every message logs as "All zones clear".
        // The high bank (zones 9-16, offsets 4/5) is an unverified extrapolation — only
        // tested against a standard 8-zone ESL-2. See docs/protocol_wire_format.md.
        bool clear = true;
        for (uint8_t zone_index = 0; zone_index < 16; zone_index++) {
          const uint8_t zone_number = zone_index + 1;
          const uint8_t bit_mask = static_cast<uint8_t>(1U << (zone_index % 8));
          const bool high_bank = zone_index >= 8;
          const size_t active_idx = high_bank ? 4 : 1;
          const size_t alarmed_idx = high_bank ? 5 : 2;
          const size_t bypassed_idx = high_bank ? 6 : 3;

          bool triggered = (active_idx < data.size()) && ((data[active_idx] & bit_mask) != 0);
          bool triggered_alarmed = (alarmed_idx < data.size()) && ((data[alarmed_idx] & bit_mask) != 0);
          bool bypassed = (bypassed_idx < data.size()) && ((data[bypassed_idx] & bit_mask) != 0);

          // No `break` after a match: register_zone()/register_zone_bypass_switch() normally
          // merge into a single zones_ entry per zone number, but don't rely on that holding
          // for every possible YAML combination (e.g. `zones:` plus a standalone binary_sensor/
          // switch for the same zone) — update every matching entry so none is silently skipped.
          for (CrowAlarmPanelZone &zone : this->zones_) {
            if (zone.zone != zone_number) {
              continue;
            }
            if (zone.motion_binary_sensor != nullptr) {
              zone.motion_binary_sensor->publish_state(triggered | triggered_alarmed);
            }
            if (zone.bypass_switch != nullptr && !is_full_broadcast) {
              zone.bypass_switch->publish_state(bypassed);
            }
          }

          if (triggered) {
            ESP_LOGD(TAG, "[%-*s] Zone %d active", this->keypad_label_width_, CONTROLLER_LABEL, zone_number);
            if (this->armed_state_ != nullptr && this->armed_state_->state != "arming") {
              this->armed_state_->publish_state("disarmed");  // Assume disarmed if motion detected in this byte
            }
            if (this->alarm_control_panel_ != nullptr &&
                this->alarm_control_panel_->get_state() != alarm_control_panel::ACP_STATE_ARMING) {
              this->last_confirmed_acp_state_ = alarm_control_panel::ACP_STATE_DISARMED;
              this->alarm_control_panel_->publish_state(alarm_control_panel::ACP_STATE_DISARMED);
            }
            clear = false;
          }
          if (triggered_alarmed) {
            if (this->armed_state_ != nullptr) {
              this->armed_state_->publish_state("pending");
            }
            if (this->alarm_control_panel_ != nullptr) {
              this->last_confirmed_acp_state_ = alarm_control_panel::ACP_STATE_PENDING;
              this->alarm_control_panel_->publish_state(alarm_control_panel::ACP_STATE_PENDING);
            }
            ESP_LOGD(TAG, "[%-*s] Alarm pending from zone %d", this->keypad_label_width_, CONTROLLER_LABEL,
                     zone_number);
            clear = false;
          }
        }
        if (clear) {
          ESP_LOGD(TAG, "[%-*s] All zones clear", this->keypad_label_width_, CONTROLLER_LABEL);
        }
        break;
      }
      case ARMED_STATE: {
        if (data.size() < 2) {
          ESP_LOGW(TAG, "[%-*s] Armed state too short, discarding", this->keypad_label_width_, CONTROLLER_LABEL);
          break;
        }
        if (data[0] == 0x00 && data[1] == 0x01) {
          if (this->armed_state_ != nullptr) {
            this->armed_state_->publish_state("arming");
          }
          ESP_LOGD(TAG, "[%-*s] Arming [%02x.%s]", this->keypad_label_width_, CONTROLLER_LABEL, type,
                   format_hex_pretty(data).c_str());
          this->last_confirmed_acp_state_ = alarm_control_panel::ACP_STATE_ARMING;
          if (this->alarm_control_panel_ != nullptr) {
            this->alarm_control_panel_->publish_state(alarm_control_panel::ACP_STATE_ARMING);
          }
        } else if (data[0] == 0x01 && data[1] == 0x00) {
          if (this->armed_state_ != nullptr) {
            this->armed_state_->publish_state("armed_away");
          }
          ESP_LOGD(TAG, "[%-*s] Armed Away [%02x.%s]", this->keypad_label_width_, CONTROLLER_LABEL, type,
                   format_hex_pretty(data).c_str());
          this->last_confirmed_acp_state_ = alarm_control_panel::ACP_STATE_ARMED_AWAY;
          if (this->alarm_control_panel_ != nullptr) {
            this->alarm_control_panel_->publish_state(alarm_control_panel::ACP_STATE_ARMED_AWAY);
          }
        } else if (data[0] == 0x00 && data[1] == 0x00) {
          if (this->armed_state_ != nullptr) {
            this->armed_state_->publish_state("disarmed");
          }
          ESP_LOGD(TAG, "[%-*s] Disarmed [%02x.%s]", this->keypad_label_width_, CONTROLLER_LABEL, type,
                   format_hex_pretty(data).c_str());
          this->last_confirmed_acp_state_ = alarm_control_panel::ACP_STATE_DISARMED;
          if (this->alarm_control_panel_ != nullptr) {
            this->alarm_control_panel_->publish_state(alarm_control_panel::ACP_STATE_DISARMED);
          }
        } else {
          ESP_LOGD(TAG, "[%-*s] Armed state unknown [%02x.%s]", this->keypad_label_width_, CONTROLLER_LABEL, type,
                   format_hex_pretty(data).c_str());
        }
        // ARMED_STATE is the controller's own authoritative state broadcast, independent of the
        // arm/disarm state machine below — this is the only signal CODE_ENTER_PENDING treats as
        // proof of success (see the enum comment in crow_alarm_panel.h for why the KEYPAD_COMMAND
        // byte[1] value below isn't trusted for this). Any recognized ARMED_STATE broadcast while
        // a terminal key is outstanding settles it, regardless of which of the three it is —
        // "Arming" confirms an arm-with-code request as much as "Armed Away"/"Disarmed" would.
        if (this->arm_disarm_state_ == ArmDisarmState::CODE_ENTER_PENDING &&
            ((data[0] == 0x00 && data[1] == 0x01) || (data[0] == 0x01 && data[1] == 0x00) ||
             (data[0] == 0x00 && data[1] == 0x00))) {
          ESP_LOGD(TAG, "Code sequence: complete (confirmed via ARMED_STATE broadcast)");
          this->arm_disarm_state_ = ArmDisarmState::IDLE;
        }
        break;
      }
      case OUTPUT_SELECT_ACK: {
        if (data.empty()) {
          ESP_LOGW(TAG, "Output-select ack too short, discarding");
          break;
        }
        CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        ESP_LOGD(TAG, "[%-*s] Output-select ACK [%02x.%s]", this->keypad_label_width_,
                 keypad_label(keypad, data[0]).c_str(), type, format_hex_pretty(data).c_str());
        if (data[0] == this->keypad_address_ &&
            this->output_select_state_ == OutputSelectState::OUTPUT_PENDING) {
          ESP_LOGD(TAG, "Output-select: ACK received, awaiting KEYPAD_COMMAND");
          this->output_select_state_ = OutputSelectState::AWAIT_COMMAND;
          this->output_select_state_enter_ms_ = millis();
        }
        break;
      }
      case KEYPRESS: {
        if (data.size() < 2) {
          ESP_LOGW(TAG, "Keypress too short, discarding");
          break;
        }
        uint8_t key = data[1];
        if (key >= sizeof(KEYS) / sizeof(KEYS[0])) {
          ESP_LOGW(TAG, "Unknown key index %d, discarding", key);
          break;
        }
        CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        ESP_LOGD(TAG, "[%-*s] Key %s (%d) pressed [%02x.%s]", this->keypad_label_width_,
                 keypad_label(keypad, data[0]).c_str(), KEYS[key], key, type, format_hex_pretty(data).c_str());
        break;
      }
      case CURRENT_TIME: {
        if (data.size() < 7) {
          ESP_LOGW(TAG, "[%-*s] Current time too short, discarding [%02x.%s]", this->keypad_label_width_,
                   CONTROLLER_LABEL, type, format_hex_pretty(data).c_str());
          break;
        }
        if (data[0] == 0 || data[0] > 7) {
          ESP_LOGW(TAG, "[%-*s] Current time has invalid day index %d [%02x.%s]", this->keypad_label_width_,
                   CONTROLLER_LABEL, data[0], type, format_hex_pretty(data).c_str());
          break;
        }
        const char *day_of_week = DAYS[data[0] - 1];
        uint16_t minutes_since_midnight = (static_cast<uint16_t>(data[1]) << 8) | data[2];
        uint8_t hour = minutes_since_midnight / 60;
        uint8_t minute = minutes_since_midnight % 60;
        if (hour >= 24) {
          ESP_LOGW(TAG, "[%-*s] Current time has invalid minutes-since-midnight value %u [%02x.%s]",
                   this->keypad_label_width_, CONTROLLER_LABEL, minutes_since_midnight, type,
                   format_hex_pretty(data).c_str());
          break;
        }
        if (data[3] >= 60) {
          ESP_LOGW(TAG, "[%-*s] Current time has invalid seconds value %u [%02x.%s]", this->keypad_label_width_,
                   CONTROLLER_LABEL, data[3], type, format_hex_pretty(data).c_str());
          break;
        }
        uint8_t day = data[4];
        uint8_t month = data[5];
        uint8_t year = data[6];
        if (day == 0 || day > 31 || month == 0 || month > 12) {
          // The documented CURRENT_TIME bit-corruption glitch (protocol_investigations.md) shifts
          // day/month/year one bit left together (a single spurious 0 bit inserted right after the
          // seconds byte) — i.e. each is exactly double its true value. Halving all three and
          // cross-checking the recovered date's weekday against the untouched day_of_week field
          // (data[0], from earlier in the frame, before the glitch's insertion point) makes a false
          // recovery astronomically unlikely, addressing the coincidental-valid-range risk noted in
          // that doc. Validated against real HA log timestamps in the 2026-08-05 traces: the
          // recovered date matched the true date/time exactly in every sample checked.
          bool recovered = false;
          if ((data[4] % 2) == 0 && (data[5] % 2) == 0 && (data[6] % 2) == 0) {
            uint8_t rec_day = data[4] / 2;
            uint8_t rec_month = data[5] / 2;
            uint8_t rec_year = data[6] / 2;
            if (rec_day >= 1 && rec_day <= 31 && rec_month >= 1 && rec_month <= 12 &&
                day_of_week_from_date(2000 + rec_year, rec_month, rec_day) == data[0]) {
              ESP_LOGI(TAG,
                       "[%-*s] Current time: recovered doubled-bit glitch, using 20%02u-%02u-%02u [%02x.%s]",
                       this->keypad_label_width_, CONTROLLER_LABEL, rec_year, rec_month, rec_day, type,
                       format_hex_pretty(data).c_str());
              day = rec_day;
              month = rec_month;
              year = rec_year;
              recovered = true;
            }
          }
          if (!recovered) {
            ESP_LOGW(TAG, "[%-*s] Current time has invalid day/month value %u/%u [%02x.%s]",
                     this->keypad_label_width_, CONTROLLER_LABEL, data[4], data[5], type,
                     format_hex_pretty(data).c_str());
            break;
          }
        }
        ESP_LOGD(TAG, "[%-*s] Controller time update: %s 20%02d-%02d-%02d %02d:%02d:%02d",
                 this->keypad_label_width_, CONTROLLER_LABEL, day_of_week, year, month, day, hour, minute, data[3]);
        break;
      }
      case RESPONSE_TIME:
        if (data.size() < 3) {
          ESP_LOGW(TAG, "[%-*s] Response time too short, discarding", this->keypad_label_width_, CONTROLLER_LABEL);
          break;
        }
        ESP_LOGD(TAG, "[%-*s] Current time setting received [%d]", this->keypad_label_width_, CONTROLLER_LABEL,
                 (data[1] << 8) | data[2]);
        break;
      case KEYPAD_COMMAND: {
        if (data.empty()) {
          ESP_LOGW(TAG, "Keypad command too short, discarding");
          break;
        }
        CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        ESP_LOGD(TAG, "[%-*s] Command [%02x.%s]", this->keypad_label_width_, keypad_label(keypad, data[0]).c_str(),
                 type, format_hex_pretty(data).c_str());
        // Drive output-select state machine forward when command is addressed to us.
        if (data[0] == this->keypad_address_) {
          switch (this->output_select_state_) {
            case OutputSelectState::OUTPUT_PENDING:
              // Controller already has us in output-select mode (KEYPAD_COMMAND arrived
              // instead of the expected 0x1D ACK). This happens when the previous sequence
              // left the controller with our keypad stuck in output-select mode because
              // the KEYPAD_COMMAND [15] exit reply was lost. Treat it the same as ACK.
              ESP_LOGW(TAG, "Output-select: KEYPAD_COMMAND in OUTPUT_PENDING (no 0x1D), recovering");
              this->output_select_state_ = OutputSelectState::AWAIT_COMMAND;
              this->output_select_state_enter_ms_ = millis();
              [[fallthrough]];
            case OutputSelectState::AWAIT_COMMAND:
            case OutputSelectState::DIGIT_PENDING:
              if (this->output_select_key_idx_ < this->output_select_keys_.size()) {
                uint8_t key = this->output_select_keys_[this->output_select_key_idx_++];
                ESP_LOGD(TAG, "Output-select: sending digit %u", key);
                this->keypress(key);
                this->output_select_state_ = OutputSelectState::DIGIT_PENDING;
                this->output_select_state_enter_ms_ = millis();
              } else {
                // All digits confirmed. Delay before sending ENTER so the controller's
                // output-fire broadcast clears the bus — avoids the ISR was_transmitting_
                // reset discarding the KEYPAD_COMMAND [15] (exit output-select) reply.
                ESP_LOGD(TAG, "Output-select: digits done, waiting %u ms before ENTER",
                         (unsigned) CrowAlarmPanelStore::OUTPUT_SELECT_ENTER_DELAY_MS);
                this->output_select_state_ = OutputSelectState::ENTER_DELAY;
                this->output_select_state_enter_ms_ = millis();
              }
              break;
            case OutputSelectState::ENTER_PENDING:
              ESP_LOGD(TAG, "Output-select: sequence complete");
              this->output_select_state_ = OutputSelectState::IDLE;
              break;
            default:
              break;
          }
          // Drive arm/disarm state machine forward.
          switch (this->arm_disarm_state_) {
            case ArmDisarmState::ARM_AWAY_PENDING:
            case ArmDisarmState::ARM_STAY_PENDING:
              ESP_LOGD(TAG, "Arm/stay: CMD received, sequence complete");
              this->arm_disarm_state_ = ArmDisarmState::IDLE;
              break;
            case ArmDisarmState::CODE_DIGIT_PENDING: {
              uint8_t cmd_byte = (data.size() > 1) ? data[1] : 0;
              if (!this->arm_disarm_digit_ack_byte_set_) {
                // Learn this sequence's "digit accepted, more expected" display_code from the
                // first digit's response, for diagnostics only. Trace: logs-7/logs-33 (2026-07-08)
                // show address 0x05 consistently getting 0x07 here — never 0x01 — across four
                // separate arm and disarm attempts with a code independently confirmed correct via
                // the physical IP keypad (0x07), while address 0x07 consistently got 0x01. The
                // controller does not validate code correctness per digit (only at ENTER), so this
                // is a per-keypad-type display quirk, not a rejection. A mid-sequence change isn't
                // treated as an abort-worthy anomaly either (see docs/arm_disarm_state_machine.md):
                // its meaning was never established, and logs-18 showed the abort itself causing a
                // stuck alarm_control_panel entity for a sequence that may well have succeeded.
                // Success/failure comes solely from the ARMED_STATE broadcast or the shared 1s
                // watchdog, same as CODE_ENTER_PENDING.
                this->arm_disarm_digit_ack_byte_ = cmd_byte;
                this->arm_disarm_digit_ack_byte_set_ = true;
              } else if (cmd_byte != this->arm_disarm_digit_ack_byte_) {
                ESP_LOGD(TAG, "Arm/disarm: CMD byte changed from 0x%02X to 0x%02X in CODE_DIGIT_PENDING, continuing",
                         this->arm_disarm_digit_ack_byte_, cmd_byte);
              }
              if (this->arm_disarm_code_idx_ < this->arm_disarm_code_digits_.size()) {
                uint8_t digit = this->arm_disarm_code_digits_[this->arm_disarm_code_idx_++];
                ESP_LOGD(TAG, "Code sequence: sending digit %u (index %u)", digit,
                         this->arm_disarm_code_idx_ - 1);
                this->keypress(digit);
                this->arm_disarm_state_enter_ms_ = millis();
              } else {
                ESP_LOGD(TAG, "Code sequence: sending terminal key 0x%02X",
                         this->arm_disarm_terminal_key_);
                this->keypress(this->arm_disarm_terminal_key_);
                this->arm_disarm_state_ = ArmDisarmState::CODE_ENTER_PENDING;
                this->arm_disarm_state_enter_ms_ = millis();
              }
              break;
            }
            case ArmDisarmState::CODE_ENTER_PENDING: {
              // byte[1] here is deliberately not used to judge success/failure — see the enum
              // comment in crow_alarm_panel.h for why (logs-10/35, logs-12/37, 2026-07-12). This
              // KEYPAD_COMMAND only proves the controller is still responding; only the
              // independent ARMED_STATE broadcast (below) or the shared 1s watchdog resolves
              // this state.
              uint8_t cmd_byte = (data.size() > 1) ? data[1] : 0;
              ESP_LOGD(TAG, "Code sequence: CMD 0x%02X after terminal key, awaiting ARMED_STATE confirmation",
                       cmd_byte);
              break;
            }
            default:
              break;
          }
          // Drive zone-bypass state machine forward.
          // See docs/zone_bypass_state_machine.md for observed timing.
          switch (this->zone_bypass_state_) {
            case ZoneBypassState::BYPASS_PENDING:
              // Only advance on our own keypad's KEYPAD_COMMAND. Other keypads carry their
              // own per-keypad BB values (e.g. the IP keypad always shows BB=0x08) that are
              // unrelated to our bypass session state.
              if (data[0] != this->keypad_address_) break;
              // Capture the session BB from this KC (reflects existing bypasses when started
              // within the panel's session-preservation window, ~20–30 s). Send all digits
              // and ENTER back-to-back; the physical keypad does the same and traces confirm
              // the panel processes them correctly without a second KC wait.
              this->zone_bypass_initial_bitmap_ = (data.size() > 3) ? data[3] : 0;
              ESP_LOGD(TAG, "[%-*s] Zone-bypass: KC received (BB=0x%02x), sending digits+ENTER",
                       this->keypad_label_width_, keypad_label(keypad, data[0]).c_str(),
                       this->zone_bypass_initial_bitmap_);
              while (this->zone_bypass_key_idx_ < this->zone_bypass_keys_.size()) {
                this->keypress(this->zone_bypass_keys_[this->zone_bypass_key_idx_++]);
              }
              this->keypress(KEY_ENTER);
              this->zone_bypass_state_ = ZoneBypassState::ENTER_PENDING;
              this->zone_bypass_state_enter_ms_ = millis();
              break;
            case ZoneBypassState::ENTER_PENDING:
              ESP_LOGD(TAG, "[%-*s] Zone-bypass: sequence complete for zone %u (KEYPAD_COMMAND after ENTER)",
                       this->keypad_label_width_, keypad_label(keypad, data[0]).c_str(),
                       this->zone_bypass_target_zone_);
              this->zone_bypass_state_ = ZoneBypassState::IDLE;
              break;
            default:
              break;
          }
        }
        break;
      }
      case KEYPAD_STATE: {
        if (data.size() < 2) {
          ESP_LOGW(TAG, "Keypad state too short, discarding");
          break;
        }
        CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        if (data[1] == 00) {
          ESP_LOGD(TAG, "[%-*s] In normal state [%02x.%s]", this->keypad_label_width_,
                   keypad_label(keypad, data[0]).c_str(), type, format_hex_pretty(data).c_str());
        } else if (data[1] == 02) {
          ESP_LOGD(TAG, "[%-*s] In installer mode [%02x.%s]", this->keypad_label_width_,
                   keypad_label(keypad, data[0]).c_str(), type, format_hex_pretty(data).c_str());
        } else if (data[1] == 03) {
          if (data.size() < 3) {
            ESP_LOGW(TAG, "Keypad programming state too short, discarding");
            break;
          }
          ESP_LOGD(TAG, "[%-*s] Programming %d [%02x.%s]", this->keypad_label_width_,
                   keypad_label(keypad, data[0]).c_str(), data[2], type, format_hex_pretty(data).c_str());
        } else {
          ESP_LOGD(TAG, "[%-*s] State unknown [%02x.%s]", this->keypad_label_width_,
                   keypad_label(keypad, data[0]).c_str(), type, format_hex_pretty(data).c_str());
        }
        break;
      }
      case KEYPAD_PING: {
        if (data.empty()) {
          ESP_LOGW(TAG, "Keypad ping too short, discarding");
          break;
        }
        CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        ESP_LOGD(TAG, "[%-*s] Ping [%02x.%s]", this->keypad_label_width_, keypad_label(keypad, data[0]).c_str(), type,
                 format_hex_pretty(data).c_str());
        if (data[0] == this->keypad_address_)
          this->last_ping_ms_ = millis();
        break;
      }
      case KEYPAD_REGISTRATION: {
        if (data.empty()) {
          ESP_LOGW(TAG, "Keypad registration too short, discarding");
          break;
        }
        CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        ESP_LOGI(TAG, "[%-*s] Registration [%02x.%s]", this->keypad_label_width_,
                 keypad_label(keypad, data[0]).c_str(), type, format_hex_pretty(data).c_str());
        // Other keypads re-registering indicates a controller reset. We intentionally do NOT
        // re-register here: if 0x05 is not pre-programmed in the controller, registering causes
        // a crash loop (controller adds 0x05 to polls, we can't respond inline, crash repeats).
        // Users who have 0x05 programmed in the controller will see it work correctly on boot.
        break;
      }
      case SETTING_VALUE: {
        if (data.size() < 5) {
          ESP_LOGW(TAG, "[%-*s] Setting value too short, discarding", this->keypad_label_width_, CONTROLLER_LABEL);
          break;
        }
        ESP_LOGD(TAG, "[%-*s] Address %d-%d has options: %s [%02x.%s]", this->keypad_label_width_, CONTROLLER_LABEL,
                 data[3], data[4], binary_indices(data[2]).c_str(), type, format_hex_pretty(data).c_str());
        break;
      }
      case SETTING_VALUE2: {
        if (data.size() < 4) {
          ESP_LOGW(TAG, "[%-*s] Setting value 2 too short, discarding", this->keypad_label_width_, CONTROLLER_LABEL);
          break;
        }
        // CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        ESP_LOGD(TAG, "[%-*s] Address %d-%d has value %d [%02x.%s]", this->keypad_label_width_, CONTROLLER_LABEL,
                 data[2], data[3], data[1], type, format_hex_pretty(data).c_str());
        break;
      }
      case SETTING_VALUE3: {
        if (data.size() < 5) {
          ESP_LOGW(TAG, "[%-*s] Setting value 3 too short, discarding", this->keypad_label_width_, CONTROLLER_LABEL);
          break;
        }
        // CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        ESP_LOGD(TAG, "[%-*s] Address %d-%d has value %d [%02x.%s]", this->keypad_label_width_, CONTROLLER_LABEL,
                 data[3], data[4], (data[1] << 8) | data[2], type, format_hex_pretty(data).c_str());
        break;
      }
      case MEMORY_EVENT: {
        if (data.size() < 2) {
          ESP_LOGW(TAG, "[%-*s] Memory event too short, discarding", this->keypad_label_width_, CONTROLLER_LABEL);
          break;
        }
        // CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        ESP_LOGD(TAG, "[%-*s] Memory event #%d ", this->keypad_label_width_, CONTROLLER_LABEL, data[1]);
        break;
      }
      case BYPASS_STATUS: {
        if (data.size() < 2) {
          ESP_LOGW(TAG, "Bypass status too short, discarding");
          break;
        }
        {
          CrowAlarmPanelKeypad bs_keypad = this->find_keypad_(data[0]);
          ESP_LOGD(TAG, "[%-*s] Bypass status: bitmap=0x%02x [%02x.%s]", this->keypad_label_width_,
                   keypad_label(bs_keypad, data[0]).c_str(), data[1], type, format_hex_pretty(data).c_str());
        }
        // After ENTER the controller may send BYPASS_STATUS before KEYPAD_COMMAND (CC=0x15).
        // Use it as a fallback completion signal for ENTER_PENDING — but only when the bitmap
        // has actually changed. An unchanged bitmap means the bypass sequence didn't land
        // (e.g. digits arrived late and were ignored by the controller).
        if (data[0] == this->keypad_address_ &&
            this->zone_bypass_state_ == ZoneBypassState::ENTER_PENDING &&
            data[1] != this->zone_bypass_initial_bitmap_) {
          CrowAlarmPanelKeypad my_keypad = this->find_keypad_(this->keypad_address_);
          ESP_LOGD(TAG, "[%-*s] Zone-bypass: sequence complete for zone %u (BYPASS_STATUS bitmap 0x%02x→0x%02x)",
                   this->keypad_label_width_, keypad_label(my_keypad, this->keypad_address_).c_str(),
                   this->zone_bypass_target_zone_, this->zone_bypass_initial_bitmap_, data[1]);
          this->zone_bypass_state_ = ZoneBypassState::IDLE;
        }
        break;
      }
      default:
        ESP_LOGD(TAG, "Unknown [%02x.%s]", type, format_hex_pretty(data).c_str());
        break;
    }
    this->on_message_trigger_->trigger(type, data);
  }

  // Output-select ENTER_DELAY: send ENTER after a short pause so the controller's
  // output-fire broadcast clears the bus before our ENTER TX starts.
  if (this->output_select_state_ == OutputSelectState::ENTER_DELAY) {
    const uint32_t now_ms = millis();
    if (now_ms - this->output_select_state_enter_ms_ >= CrowAlarmPanelStore::OUTPUT_SELECT_ENTER_DELAY_MS) {
      ESP_LOGD(TAG, "Output-select: sending ENTER (after %u ms delay)",
               (unsigned)(now_ms - this->output_select_state_enter_ms_));
      this->keypress(KEY_ENTER);
      this->output_select_state_ = OutputSelectState::ENTER_PENDING;
      this->output_select_state_enter_ms_ = millis();
    }
  }

  // Output-select watchdog: abort if any non-IDLE state exceeds 1s without progress.
  // States before output fires (OUTPUT_PENDING, AWAIT_COMMAND, DIGIT_PENDING) get one
  // automatic retry; states after output fires (ENTER_DELAY, ENTER_PENDING) abort
  // immediately to avoid double-firing the output.
  if (this->output_select_state_ != OutputSelectState::IDLE) {
    const uint32_t now_ms = millis();
    if (now_ms - this->output_select_state_enter_ms_ > 1000) {
      const bool pre_fire = (this->output_select_state_ == OutputSelectState::OUTPUT_PENDING ||
                             this->output_select_state_ == OutputSelectState::AWAIT_COMMAND ||
                             this->output_select_state_ == OutputSelectState::DIGIT_PENDING);
      if (pre_fire && this->output_select_retry_count_ == 0) {
        ESP_LOGW(TAG, "Output-select: timeout in state %u, retrying (output select)",
                 static_cast<uint8_t>(this->output_select_state_));
        this->output_select_retry_count_++;
        this->output_select_key_idx_ = 0;
        this->output_select_state_ = OutputSelectState::OUTPUT_PENDING;
        this->output_select_state_enter_ms_ = millis();
        this->keypress(KEY_OUTPUT);
      } else {
        ESP_LOGW(TAG, "Output-select: timeout in state %u, aborting",
                 static_cast<uint8_t>(this->output_select_state_));
        this->output_select_state_ = OutputSelectState::IDLE;
        this->output_select_keys_.clear();
        this->output_select_key_idx_ = 0;
        this->output_select_retry_count_ = 0;
      }
    }
  }

  // Zone-bypass watchdog: abort if any non-IDLE state exceeds 1s without progress.
  // No automatic retry — the sequence toggles panel state, so a blind retry could
  // undo a toggle that actually landed (same reasoning as post-fire output states).
  if (this->zone_bypass_state_ != ZoneBypassState::IDLE) {
    const uint32_t now_ms = millis();
    if (now_ms - this->zone_bypass_state_enter_ms_ > 1000) {
      CrowAlarmPanelKeypad my_keypad = this->find_keypad_(this->keypad_address_);
      ESP_LOGW(TAG, "[%-*s] Zone-bypass: timeout in state %u for zone %u, aborting", this->keypad_label_width_,
               keypad_label(my_keypad, this->keypad_address_).c_str(),
               static_cast<uint8_t>(this->zone_bypass_state_), this->zone_bypass_target_zone_);
      this->zone_bypass_state_ = ZoneBypassState::IDLE;
      this->zone_bypass_keys_.clear();
      this->zone_bypass_key_idx_ = 0;
    }
  }

  // Arm/disarm watchdog: abort if any non-IDLE state exceeds 1s without progress. See
  // ARM_DISARM_MAX_RETRIES in crow_alarm_panel.h for why retrying here (unlike output-select/
  // zone-bypass above) is safe: a timeout reliably means the panel's state did not change.
  if (this->arm_disarm_state_ != ArmDisarmState::IDLE) {
    const uint32_t now_ms = millis();
    if (now_ms - this->arm_disarm_state_enter_ms_ > 1000) {
      if (this->arm_disarm_retry_count_ < ARM_DISARM_MAX_RETRIES) {
        this->arm_disarm_retry_count_++;
        ESP_LOGW(TAG, "Arm/disarm: timeout in state %u, retrying (%u/%u)",
                 static_cast<uint8_t>(this->arm_disarm_state_), this->arm_disarm_retry_count_,
                 ARM_DISARM_MAX_RETRIES);
        switch (this->arm_disarm_state_) {
          case ArmDisarmState::ARM_AWAY_PENDING:
            this->keypress(KEY_ARM);
            break;
          case ArmDisarmState::ARM_STAY_PENDING:
            this->keypress(KEY_STAY);
            break;
          case ArmDisarmState::CODE_DIGIT_PENDING:
          case ArmDisarmState::CODE_ENTER_PENDING:
            // Restart the whole code+terminal-key sequence from scratch, exactly like a fresh
            // manual retry (proven reliable across every session in arm_disarm_state_machine.md).
            this->arm_disarm_code_idx_ = 1;
            this->arm_disarm_state_ = ArmDisarmState::CODE_DIGIT_PENDING;
            this->arm_disarm_digit_ack_byte_set_ = false;
            this->keypress(this->arm_disarm_code_digits_[0]);
            break;
          default:
            break;
        }
        this->arm_disarm_state_enter_ms_ = millis();
      } else {
        ESP_LOGW(TAG, "Arm/disarm: timeout in state %u, aborting after %u retries",
                 static_cast<uint8_t>(this->arm_disarm_state_), this->arm_disarm_retry_count_);
        this->arm_disarm_state_ = ArmDisarmState::IDLE;
        this->arm_disarm_code_digits_.clear();
        this->arm_disarm_code_idx_ = 0;
        this->arm_disarm_retry_count_ = 0;
        // CrowAlarmControlPanel::control() optimistically publishes ACP_STATE_ARMING/DISARMING
        // before this sequence resolves. On abort no ARMED_STATE broadcast is coming to correct
        // that, so without this the entity would be stuck in the transitional state forever,
        // rejecting both future arm and disarm calls (ESPHome's alarm_control_panel validate_()
        // requires DISARMED to arm and an armed/pending state to disarm). Restore it to the last
        // state the controller itself actually confirmed.
        if (this->alarm_control_panel_ != nullptr) {
          this->alarm_control_panel_->publish_state(this->last_confirmed_acp_state_);
        }
      }
    }
  }

  if (this->is_active_keypad()) {
    // After a startup delay, announce ourselves to the controller as a registered keypad.
    // Payload {0x00} → A0 <addr> 00  (physical keypad style).
    const uint32_t now_ms = millis();
    // Watchdog: if we were being polled but haven't heard from the controller in 60 s,
    // re-send registration (handles controller resets where physical keypads re-register).
    if (this->registration_sent_ && this->last_ping_ms_ != 0 &&
        (now_ms - this->last_ping_ms_) >= 60000) {
      ESP_LOGW(TAG, "No ping for 60 s, re-sending registration announce");
      this->registration_sent_ = false;
    }
    if (!this->registration_sent_ && now_ms >= this->registration_after_ms_ && this->is_bus_idle_()) {
      CrowAlarmPanelKeypad keypad = this->find_keypad_(this->keypad_address_);
      ESP_LOGW(TAG, "[%-*s] Sending registration announce", this->keypad_label_width_,
               keypad_label(keypad, this->keypad_address_).c_str());
      this->send_packet(KEYPAD_REGISTRATION, {0x00});
      this->registration_sent_ = true;
    }
  }
}

void CrowAlarmPanel::start_code_sequence_(const std::string &code, uint8_t terminal_key) {
  this->arm_disarm_code_digits_.clear();
  for (char c : code) {
    if (c >= '0' && c <= '9') {
      this->arm_disarm_code_digits_.push_back(c - '0');
    }
  }
  if (this->arm_disarm_code_digits_.empty()) {
    ESP_LOGW(TAG, "start_code_sequence_: empty or non-numeric code, ignoring");
    return;
  }
  ESP_LOGD(TAG, "Code sequence: %u digits, terminal key 0x%02X",
           this->arm_disarm_code_digits_.size(), terminal_key);
  this->arm_disarm_terminal_key_ = terminal_key;
  // idx=1 and state set BEFORE keypress — same race-condition prevention as set_output().
  this->arm_disarm_code_idx_ = 1;
  this->arm_disarm_state_ = ArmDisarmState::CODE_DIGIT_PENDING;
  this->arm_disarm_state_enter_ms_ = millis();
  this->arm_disarm_retry_count_ = 0;
  this->arm_disarm_digit_ack_byte_set_ = false;
  this->keypress(this->arm_disarm_code_digits_[0]);
}

void CrowAlarmPanel::arm_away(const std::string &code) {
  if (!this->is_active_keypad()) {
    ESP_LOGW(TAG, "arm_away: passive monitor mode, ignoring");
    return;
  }
  if (this->arm_disarm_state_ != ArmDisarmState::IDLE) {
    ESP_LOGW(TAG, "arm_away: ARM/DISARM already in progress, ignoring");
    return;
  }
  if (!code.empty()) {
    ESP_LOGI(TAG, "Arm away (with code)");
    this->start_code_sequence_(code, KEY_ARM);
  } else {
    ESP_LOGI(TAG, "Arm away");
    this->arm_disarm_state_ = ArmDisarmState::ARM_AWAY_PENDING;
    this->arm_disarm_state_enter_ms_ = millis();
    this->arm_disarm_retry_count_ = 0;
    this->keypress(KEY_ARM);
  }
}

void CrowAlarmPanel::arm_stay(const std::string &code) {
  if (!this->is_active_keypad()) {
    ESP_LOGW(TAG, "arm_stay: passive monitor mode, ignoring");
    return;
  }
  if (this->arm_disarm_state_ != ArmDisarmState::IDLE) {
    ESP_LOGW(TAG, "arm_stay: ARM/DISARM already in progress, ignoring");
    return;
  }
  if (!code.empty()) {
    ESP_LOGI(TAG, "Arm stay (with code)");
    this->start_code_sequence_(code, KEY_STAY);
  } else {
    ESP_LOGI(TAG, "Arm stay");
    this->arm_disarm_state_ = ArmDisarmState::ARM_STAY_PENDING;
    this->arm_disarm_state_enter_ms_ = millis();
    this->arm_disarm_retry_count_ = 0;
    this->keypress(KEY_STAY);
  }
}

void CrowAlarmPanel::disarm(const std::string &code) {
  if (!this->is_active_keypad()) {
    ESP_LOGW(TAG, "disarm: passive monitor mode, ignoring");
    return;
  }
  if (!this->is_armed()) {
    ESP_LOGW(TAG, "disarm: not armed, ignoring");
    return;
  }
  if (this->arm_disarm_state_ != ArmDisarmState::IDLE) {
    ESP_LOGW(TAG, "disarm: ARM/DISARM already in progress, ignoring");
    return;
  }
  ESP_LOGI(TAG, "Disarm");
  this->start_code_sequence_(code, KEY_ENTER);
}

void CrowAlarmPanel::keypress(uint8_t key) {
  this->send_packet(KEYPRESS, {key});
}

void CrowAlarmPanel::set_output(uint8_t output, bool state) {
  if (!this->is_active_keypad()) {
    ESP_LOGW(TAG, "set_output(%u, %s): passive monitor mode, ignoring", output, state ? "on" : "off");
    return;
  }
  if (this->output_select_state_ != OutputSelectState::IDLE) {
    ESP_LOGW(TAG, "set_output(%u, %s): output-select sequence already in progress", output, state ? "on" : "off");
    return;
  }
  // Build digit queue consumed one per KEYPAD_COMMAND received after the ACK.
  this->output_select_keys_.clear();
  if (output >= 10) {
    this->output_select_keys_.push_back(output / 10);
  }
  this->output_select_keys_.push_back(output % 10);
  this->output_select_key_idx_ = 0;

  ESP_LOGD(TAG, "Output-select: starting sequence for output %u (%s)", output, state ? "on" : "off");
  this->output_select_retry_count_ = 0;
  // Set state BEFORE the keypress call. send_packet() uses delay()/yield() internally
  // which re-enters loop(). If the ACK (0x1D) arrives during that yield, loop() must
  // see OUTPUT_PENDING to correctly transition to AWAIT_COMMAND.
  this->output_select_state_ = OutputSelectState::OUTPUT_PENDING;
  this->output_select_state_enter_ms_ = millis();
  this->keypress(KEY_OUTPUT);
}

void CrowAlarmPanel::set_zone_bypass(uint8_t zone, bool state) {
  if (!this->is_active_keypad()) {
    ESP_LOGW(TAG, "set_zone_bypass(%u, %s): passive monitor mode, ignoring", zone, ONOFF(state));
    return;
  }
  if (this->zone_bypass_state_ != ZoneBypassState::IDLE) {
    ESP_LOGW(TAG, "set_zone_bypass(%u, %s): bypass sequence already in progress, ignoring", zone,
             ONOFF(state));
    return;
  }
  // All three keypress state machines advance on the same KEYPAD_COMMAND frames; running
  // two at once would double-consume confirmations.
  if (this->output_select_state_ != OutputSelectState::IDLE ||
      this->arm_disarm_state_ != ArmDisarmState::IDLE) {
    ESP_LOGW(TAG, "set_zone_bypass(%u, %s): another keypress sequence in progress, ignoring", zone,
             ONOFF(state));
    return;
  }
  if (this->is_armed()) {
    ESP_LOGW(TAG, "set_zone_bypass(%u, %s): panel is armed, bypass can only be toggled while disarmed",
             zone, ONOFF(state));
    return;
  }
  // The keypad sequence toggles bypass. Skip if the switch already reports the requested
  // state. The switch mirrors the ZONE_STATE bypass bitmap but defaults to off at boot,
  // so before the first broadcast an "off" request is treated as already satisfied.
  for (auto &z : this->zones_) {
    if (z.zone != zone) {
      continue;
    }
    if (z.bypass_switch != nullptr && z.bypass_switch->state == state) {
      ESP_LOGD(TAG, "set_zone_bypass(%u, %s): switch already reports requested state, skipping",
               zone, ONOFF(state));
      return;
    }
    break;
  }

  // Zone number is always entered as two digits, zero-padded (zone 2 → "0", "2").
  this->zone_bypass_keys_.clear();
  this->zone_bypass_keys_.push_back(zone / 10);
  this->zone_bypass_keys_.push_back(zone % 10);
  this->zone_bypass_key_idx_ = 0;
  this->zone_bypass_target_zone_ = zone;

  CrowAlarmPanelKeypad my_keypad = this->find_keypad_(this->keypad_address_);
  ESP_LOGD(TAG, "[%-*s] Zone-bypass: starting toggle sequence for zone %u (want %s)", this->keypad_label_width_,
           keypad_label(my_keypad, this->keypad_address_).c_str(), zone, ONOFF(state));
  // Set state BEFORE the keypress calls — send_packet() delays/yields internally, which
  // re-enters loop(); a KEYPAD_COMMAND arriving during that yield must see BYPASS_PENDING.
  this->zone_bypass_state_ = ZoneBypassState::BYPASS_PENDING;
  this->zone_bypass_state_enter_ms_ = millis();
  this->keypress(KEY_BYPASS);
  // Send digit[0] (= zone / 10, always "0" for zones 1–9) immediately, before KC arrives.
  // The physical keypad generates the entire bypass sequence (BYPASS + 0 + zone_digit +
  // ENTER) from a single zone key press. BYPASS and "0" are sent first (~17–43ms), then
  // zone_digit + ENTER follow after KC. Only send if still BYPASS_PENDING — a KC during
  // the yield above already advanced state and consumed all remaining digits inline.
  if (this->zone_bypass_state_ == ZoneBypassState::BYPASS_PENDING) {
    this->keypress(this->zone_bypass_keys_[this->zone_bypass_key_idx_++]);
  }
}

void CrowAlarmPanel::send_packet(uint8_t type, const std::vector<uint8_t> &data) {
  if (!this->is_active_keypad()) {
    ESP_LOGW(TAG, "No keypad address configured — cannot send packet (passive monitor mode)");
    return;
  }
  std::vector<uint8_t> packet;
  packet.push_back(BOUNDARY);
  packet.push_back(type);
  packet.push_back(this->keypad_address_);
  packet.insert(packet.end(), data.begin(), data.end());
  packet.push_back(BOUNDARY);

  uint32_t start_ms = millis();
  while (!this->is_bus_idle_()) {
    if (millis() - start_ms > 5000) {
      ESP_LOGW(TAG, "Timeout waiting for bus idle before sending packet");
      return;
    }
    delay(2);
    yield();
  }

  ESP_LOGD(TAG, "Sending packet: [%s]", format_hex_pretty(packet).c_str());
  this->send_packet_blocking_(packet);
  ESP_LOGD(TAG, "Packet sent");
}

bool CrowAlarmPanel::is_bus_idle_() {
  if (this->store_.inside_) {
    ESP_LOGV(TAG, "Bus not idle: inside message boundary");
    return false;
  }

  if (!this->data_pin_->digital_read()) {
    ESP_LOGV(TAG, "Bus not idle: data line low");
    return false;
  }

  uint32_t now_us = micros();
  uint32_t elapsed_us = now_us - this->store_.last_clock_time_;
  if (elapsed_us < CrowAlarmPanelStore::BUS_IDLE_TIMEOUT_US) {
    ESP_LOGV(TAG, "Bus not idle: %uus since last clock edge (need %uus)", (unsigned) elapsed_us,
             (unsigned) CrowAlarmPanelStore::BUS_IDLE_TIMEOUT_US);
    return false;
  }

  uint32_t elapsed_ms = millis() - this->store_.last_transmission_time_;
  if (elapsed_ms < CrowAlarmPanelStore::MIN_TX_INTERVAL_MS) {
    ESP_LOGV(TAG, "Bus not idle: %ums since last TX (need %ums)", (unsigned) elapsed_ms,
             (unsigned) CrowAlarmPanelStore::MIN_TX_INTERVAL_MS);
    return false;
  }

  return true;
}

bool CrowAlarmPanel::wait_for_clock_edge_(bool wait_for_state, uint32_t timeout_us) {
  const uint32_t start = micros();
  while (this->clock_pin_->digital_read() != wait_for_state) {
    if (micros() - start >= timeout_us) {
      return false;
    }
    delayMicroseconds(10);
  }
  return true;
}

void IRAM_ATTR CrowAlarmPanel::send_packet_blocking_(const std::vector<uint8_t> &packet) {
  // Set before InterruptLock so the ISR on the other core immediately sees it.
  this->store_.is_transmitting_ = true;
  {
    InterruptLock lock;

    bool bits[200];
    uint16_t bit_count = 0;
    for (uint8_t byte : packet) {
      for (uint8_t j = 0; j < 8; j++) {
        bits[bit_count++] = (byte >> j) & 1;
      }
    }

    if (!this->wait_for_clock_edge_(HIGH, CrowAlarmPanelStore::TX_START_TIMEOUT_US) ||
        !this->wait_for_clock_edge_(LOW, CrowAlarmPanelStore::TX_START_TIMEOUT_US) ||
        !this->wait_for_clock_edge_(HIGH, CrowAlarmPanelStore::TX_START_TIMEOUT_US)) {
      this->data_pin_->pin_mode(gpio::FLAG_INPUT);
      this->store_.is_transmitting_ = false;
      return;
    }

    for (uint16_t i = 0; i < bit_count; i++) {
      if (bits[i]) {
        this->data_pin_->pin_mode(gpio::FLAG_INPUT);
      } else {
        this->data_pin_->pin_mode(gpio::FLAG_OUTPUT);
        this->data_pin_->digital_write(LOW);
      }

      if (!this->wait_for_clock_edge_(LOW, CrowAlarmPanelStore::TX_BIT_TIMEOUT_US)) {
        break;
      }
      if (!this->wait_for_clock_edge_(HIGH, CrowAlarmPanelStore::TX_BIT_TIMEOUT_US)) {
        break;
      }
    }

    this->data_pin_->pin_mode(gpio::FLAG_INPUT);
    this->store_.last_transmission_time_ = millis();
  }  // InterruptLock releases here
  this->store_.is_transmitting_ = false;
}

bool CrowAlarmPanel::is_armed() const {
  if (this->armed_state_ != nullptr) {
    return this->armed_state_->state != "disarmed";
  }
  if (this->alarm_control_panel_ != nullptr) {
    return this->alarm_control_panel_->get_state() != alarm_control_panel::ACP_STATE_DISARMED;
  }
  return false;
}

void CrowAlarmPanelZoneBypassSwitch::write_state(bool state) {
  if (this->parent_ == nullptr) {
    ESP_LOGE(TAG, "Parent not set, ignoring zone bypass switch command");
    return;
  }
  // No optimistic publish: bypass is a toggle sequence on the panel, so the switch state
  // is only published from the ZONE_STATE bypass bitmap once the panel confirms it.
  this->parent_->set_zone_bypass(this->zone_number_, state);
}

void CrowAlarmPanelZoneBypassSwitch::dump_config() {
  LOG_SWITCH("", "Crow Alarm Panel Zone Bypass Switch", this);
  ESP_LOGCONFIG(TAG, "  Zone number %d", this->zone_number_);
}
}  // namespace crow_alarm_panel
}  // namespace esphome
