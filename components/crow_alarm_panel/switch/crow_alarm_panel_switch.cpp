#include "esphome/core/log.h"
#include "crow_alarm_panel_switch.h"

namespace esphome {
namespace crow_alarm_panel {

static const char *TAG = "crow_alarm_panel.switch";

void CrowAlarmPanelSwitch::dump_config() {
  LOG_SWITCH("", "Crow Alarm Panel Switch", this);
}

void CrowAlarmPanelOutputSwitch::write_state(bool state) {
  if (this->parent_ == nullptr) {
    ESP_LOGE(TAG, "Parent not set, ignoring output switch command");
    return;
  }
  this->parent_->set_output(this->output_number_, state);
  this->publish_state(state);
}

void CrowAlarmPanelOutputSwitch::dump_config() {
  LOG_SWITCH("", "Crow Alarm Panel Output Switch", this);
  ESP_LOGCONFIG(TAG, "  Output number %d", this->output_number_);
}

void CrowAlarmPanelRawLogSwitch::write_state(bool state) {
  if (this->parent_ == nullptr) {
    ESP_LOGE(TAG, "Parent not set, ignoring raw log switch command");
    return;
  }
  this->parent_->set_raw_frame_logging_enabled(state);
  this->publish_state(state);
}

void CrowAlarmPanelRawLogSwitch::dump_config() {
  LOG_SWITCH("", "Crow Alarm Panel Raw Log Switch", this);
}

void CrowAlarmPanelRawBitTraceSwitch::write_state(bool state) {
  if (this->parent_ == nullptr) {
    ESP_LOGE(TAG, "Parent not set, ignoring raw bit trace switch command");
    return;
  }
  this->parent_->set_raw_bit_trace_enabled(state);
  this->publish_state(state);
}

void CrowAlarmPanelRawBitTraceSwitch::dump_config() {
  LOG_SWITCH("", "Crow Alarm Panel Raw Bit Trace Switch", this);
}

}  // namespace crow_alarm_panel
}  // namespace esphome
