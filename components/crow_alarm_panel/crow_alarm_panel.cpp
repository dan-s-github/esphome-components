#include "crow_alarm_panel.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/gpio.h"

#define HIGH 1
#define LOW 0

namespace esphome {
namespace crow_alarm_panel {

static const char *TAG = "crow_alarm_panel";

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
  this->store_.setup(this->clock_pin_, this->data_pin_);

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

    ESP_LOGV(TAG, "Received raw frame [%02x.%s]", type, format_hex_pretty(data).c_str());

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
        ESP_LOGD(TAG, "[%s] Controller status: b1=0x%02X flags=0x%02X bits=%s b3=0x%02X b4=0x%02X profile:%s state:%s "
                      "[%02x.%s]",
                 keypad_label(keypad, data[0]).c_str(), data[1], data[2], bits.c_str(), data[3], data[4],
                 controller_status_profile(data[2]), controller_status_state(data[2]), type,
                 format_hex_pretty(data).c_str());
        break;
      }
      case OUTPUT_STATE:
        if (data.size() < 1) {
          ESP_LOGW(TAG, "Output state too short, discarding");
          break;
        }
        ESP_LOGD(TAG, "Output state [%s]", format_hex_pretty(data).c_str());
        for (CrowAlarmPanelOutput output : this->outputs_) {
          bool on = ((data[0] >> (output.number - 1)) & 0x01);
          output.the_switch->publish_state(on);
        }
        break;
      case ZONE_STATE: {
        if (data.size() < 6) {
          ESP_LOGW(TAG, "Zone state invalid length, discarding");
          return;
        }
        ESP_LOGD(TAG, "Zone state received [%s]", format_hex_pretty(data).c_str());
        bool clear = true;
        for (CrowAlarmPanelZone zone : this->zones_) {
          bool triggered = ((data[1] >> (zone.zone - 1)) & 0x01);
          bool triggered_alarmed = ((data[2] >> (zone.zone - 1)) & 0x01);
          bool bypassed = ((data[3] >> (zone.zone - 1)) & 0x01);

          if (zone.motion_binary_sensor != nullptr) {
            zone.motion_binary_sensor->publish_state(triggered | triggered_alarmed);
          }
          if (zone.bypass_binary_sensor != nullptr) {
            zone.bypass_binary_sensor->publish_state(bypassed);
          }

          if (triggered) {
            ESP_LOGD(TAG, "Zone %d active", zone.zone);
            if (this->armed_state_ != nullptr && this->armed_state_->state != "arming") {
              this->armed_state_->publish_state("disarmed");  // Assume disarmed if motion detected in this byte
            }
            if (this->alarm_control_panel_ != nullptr &&
                this->alarm_control_panel_->get_state() != alarm_control_panel::ACP_STATE_ARMING) {
              this->alarm_control_panel_->publish_state(alarm_control_panel::ACP_STATE_DISARMED);
            }
            clear = false;
          }
          if (triggered_alarmed) {
            if (this->armed_state_ != nullptr) {
              this->armed_state_->publish_state("pending");
            }
            if (this->alarm_control_panel_ != nullptr) {
              this->alarm_control_panel_->publish_state(alarm_control_panel::ACP_STATE_PENDING);
            }
            ESP_LOGD(TAG, "Alarm pending from zone %d", zone.zone);
            clear = false;
          }
        }
        if (clear) {
          ESP_LOGD(TAG, "All zones clear");
        }
        break;
      }
      case ARMED_STATE: {
        if (data.size() < 2) {
          ESP_LOGW(TAG, "Armed state too short, discarding");
          break;
        }
        if (armed_state_ != nullptr) {
          if (data[0] == 0x00 && data[1] == 0x01) {
            this->armed_state_->publish_state("arming");
            ESP_LOGD(TAG, "Arming [%02x.%s]", type, format_hex_pretty(data).c_str());
            if (this->alarm_control_panel_ != nullptr) {
              this->alarm_control_panel_->publish_state(alarm_control_panel::ACP_STATE_ARMING);
            }
          } else if (data[0] == 0x01 && data[1] == 0x00) {
            this->armed_state_->publish_state("armed_away");
            ESP_LOGD(TAG, "Armed Away [%02x.%s]", type, format_hex_pretty(data).c_str());
            if (this->alarm_control_panel_ != nullptr) {
              this->alarm_control_panel_->publish_state(alarm_control_panel::ACP_STATE_ARMED_AWAY);
            }
          } else if (data[0] == 0x00 && data[1] == 0x00) {
            this->armed_state_->publish_state("disarmed");
            ESP_LOGD(TAG, "Disarmed [%02x.%s]", type, format_hex_pretty(data).c_str());
            if (this->alarm_control_panel_ != nullptr) {
              this->alarm_control_panel_->publish_state(alarm_control_panel::ACP_STATE_DISARMED);
            }
          } else {
            ESP_LOGD(TAG, "Armed state unknown [%02x.%s]", type, format_hex_pretty(data).c_str());
          }
        }
        break;
      }
      case OUTPUT_SELECT_ACK: {
        if (data.empty()) {
          ESP_LOGW(TAG, "Output-select ack too short, discarding");
          break;
        }
        CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        ESP_LOGD(TAG, "[%s] Output-select ack [%02x.%s]", keypad_label(keypad, data[0]).c_str(), type,
                 format_hex_pretty(data).c_str());
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
        ESP_LOGD(TAG, "[%s] Key %s (%d) pressed [%02x.%s]", keypad_label(keypad, data[0]).c_str(), KEYS[key], key, type,
                 format_hex_pretty(data).c_str());
        break;
      }
      case CURRENT_TIME: {
        if (data.size() < 7) {
          ESP_LOGW(TAG, "Current time too short, discarding");
          break;
        }
        if (data[0] == 0 || data[0] > 7) {
          ESP_LOGW(TAG, "Current time has invalid day index %d", data[0]);
          break;
        }
        const char *day_of_week = DAYS[data[0] - 1];
        uint16_t minutes_since_midnight = (static_cast<uint16_t>(data[1]) << 8) | data[2];
        uint8_t hour = minutes_since_midnight / 60;
        uint8_t minute = minutes_since_midnight % 60;
        if (hour >= 24) {
          ESP_LOGW(TAG, "Current time has invalid minutes-since-midnight value %u", minutes_since_midnight);
          break;
        }
        if (data[3] >= 60) {
          ESP_LOGW(TAG, "Current time has invalid seconds value %u", data[3]);
          break;
        }
        if (data[4] == 0 || data[4] > 31) {
          ESP_LOGW(TAG, "Current time has invalid day-of-month value %u", data[4]);
          break;
        }
        if (data[5] == 0 || data[5] > 12) {
          ESP_LOGW(TAG, "Current time has invalid month value %u", data[5]);
          break;
        }
        ESP_LOGD(TAG, "Controller time update: %s 20%02d-%02d-%02d %02d:%02d:%02d", day_of_week, data[6], data[5],
                 data[4], hour, minute, data[3]);
        break;
      }
      case RESPONSE_TIME:
        if (data.size() < 3) {
          ESP_LOGW(TAG, "Response time too short, discarding");
          break;
        }
        ESP_LOGD(TAG, "Current time setting received [%d]", (data[1] << 8) | data[2]);
        break;
      case KEYPAD_COMMAND: {
        if (data.empty()) {
          ESP_LOGW(TAG, "Keypad command too short, discarding");
          break;
        }
        CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        ESP_LOGD(TAG, "[%s] Command [%02x.%s]", keypad_label(keypad, data[0]).c_str(), type,
                 format_hex_pretty(data).c_str());
        break;
      }
      case KEYPAD_STATE: {
        if (data.size() < 2) {
          ESP_LOGW(TAG, "Keypad state too short, discarding");
          break;
        }
        CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        if (data[1] == 00) {
          ESP_LOGD(TAG, "[%s] In normal state [%02x.%s]", keypad_label(keypad, data[0]).c_str(), type,
                   format_hex_pretty(data).c_str());
        } else if (data[1] == 02) {
          ESP_LOGD(TAG, "[%s] In installer mode [%02x.%s]", keypad_label(keypad, data[0]).c_str(), type,
                   format_hex_pretty(data).c_str());
        } else if (data[1] == 03) {
          if (data.size() < 3) {
            ESP_LOGW(TAG, "Keypad programming state too short, discarding");
            break;
          }
          ESP_LOGD(TAG, "[%s] Programming %d [%02x.%s]", keypad_label(keypad, data[0]).c_str(), data[2], type,
                   format_hex_pretty(data).c_str());
        } else {
          ESP_LOGD(TAG, "[%s] State unknown [%02x.%s]", keypad_label(keypad, data[0]).c_str(), type,
                   format_hex_pretty(data).c_str());
        }
        break;
      }
      case KEYPAD_PING: {
        if (data.empty()) {
          ESP_LOGW(TAG, "Keypad ping too short, discarding");
          break;
        }
        CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        ESP_LOGD(TAG, "[%s] Ping [%02x.%s]", keypad_label(keypad, data[0]).c_str(), type,
                 format_hex_pretty(data).c_str());
        break;
      }
      case KEYPAD_REGISTRATION: {
        if (data.empty()) {
          ESP_LOGW(TAG, "Keypad registration too short, discarding");
          break;
        }
        CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        ESP_LOGD(TAG, "[%s] Registration [%02x.%s]", keypad_label(keypad, data[0]).c_str(), type,
                 format_hex_pretty(data).c_str());
        // Other keypads re-registering indicates a controller reset. We intentionally do NOT
        // re-register here: if 0x05 is not pre-programmed in the controller, registering causes
        // a crash loop (controller adds 0x05 to polls, we can't respond inline, crash repeats).
        // Users who have 0x05 programmed in the controller will see it work correctly on boot.
        break;
      }
      case SETTING_VALUE: {
        if (data.size() < 5) {
          ESP_LOGW(TAG, "Setting value too short, discarding");
          break;
        }
        ESP_LOGD(TAG, "Address %d-%d has options: %s [%02x.%s]", data[3], data[4], binary_indices(data[2]).c_str(),
                 type, format_hex_pretty(data).c_str());
        break;
      }
      case SETTING_VALUE2: {
        if (data.size() < 4) {
          ESP_LOGW(TAG, "Setting value 2 too short, discarding");
          break;
        }
        // CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        ESP_LOGD(TAG, "Address %d-%d has value %d [%02x.%s]", data[2], data[3], data[1], type,
                 format_hex_pretty(data).c_str());
        break;
      }
      case SETTING_VALUE3: {
        if (data.size() < 5) {
          ESP_LOGW(TAG, "Setting value 3 too short, discarding");
          break;
        }
        // CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        ESP_LOGD(TAG, "Address %d-%d has value %d [%02x.%s]", data[3], data[4], (data[1] << 8) | data[2], type,
                 format_hex_pretty(data).c_str());
        break;
      }
      case MEMORY_EVENT: {
        if (data.size() < 2) {
          ESP_LOGW(TAG, "Memory event too short, discarding");
          break;
        }
        // CrowAlarmPanelKeypad keypad = this->find_keypad_(data[0]);
        ESP_LOGD(TAG, "Memory event #%d ", data[1]);
        break;
      }
      default:
        ESP_LOGD(TAG, "Unknown [%02x.%s]", type, format_hex_pretty(data).c_str());
        break;
    }
    this->on_message_trigger_->trigger(type, data);
  }
}

void CrowAlarmPanel::arm_away() {
  ESP_LOGW(TAG, "arm_away: TX disabled in listen-only mode");
}

void CrowAlarmPanel::arm_stay() {
  ESP_LOGW(TAG, "arm_stay: TX disabled in listen-only mode");
}

void CrowAlarmPanel::disarm(const std::string &code) {
  ESP_LOGW(TAG, "disarm: TX disabled in listen-only mode");
}

void CrowAlarmPanel::set_output(uint8_t output, bool state) {
  ESP_LOGW(TAG, "set_output(%u, %s): TX disabled in listen-only mode", output, state ? "on" : "off");
}

void CrowAlarmPanel::send_packet(uint8_t type, const std::vector<uint8_t> &data) {
  ESP_LOGW(TAG, "send_packet(0x%02x): TX disabled in listen-only mode", type);
}

void CrowAlarmPanel::keypress(uint8_t key) {
  ESP_LOGW(TAG, "keypress(%s): TX disabled in listen-only mode", KEYS[key]);
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
}  // namespace crow_alarm_panel
}  // namespace esphome
