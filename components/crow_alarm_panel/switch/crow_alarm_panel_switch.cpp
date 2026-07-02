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

}  // namespace crow_alarm_panel
}  // namespace esphome
