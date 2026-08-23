#include "crow_alarm_panel.h"
#include "esphome/core/log.h"

namespace esphome {
namespace crow_alarm_panel {

static const char *TAG_ACP = "crow_alarm_panel.acp";

void CrowAlarmControlPanel::dump_config() { ESP_LOGCONFIG(TAG_ACP, "Crow Alarm Control Panel"); }

void CrowAlarmControlPanel::control(const alarm_control_panel::AlarmControlPanelCall &call) {
  if (this->parent_ == nullptr) {
    ESP_LOGE(TAG_ACP, "Parent not set, ignoring control call");
    return;
  }

  const auto target_state = call.get_state();
  if (!target_state.has_value()) {
    ESP_LOGW(TAG_ACP, "No target state in control call");
    return;
  }

  switch (*target_state) {
    case alarm_control_panel::ACP_STATE_ARMED_AWAY: {
      std::string code = this->requires_code_to_arm_ ? this->resolve_code_(call) : "";
      if (this->requires_code_to_arm_ && code.empty()) {
        this->status_momentary_warning("Code required to arm", 2000);
        return;
      }
      // Publish the optimistic transitional state only if the request was accepted (a
      // rejected request starts no sequence and no watchdog, so nothing would ever move the
      // entity out of ARMING again) AND this call's operation is still the active one. The
      // call can yield waiting for the bus, during which the operation may resolve (matching
      // ARMED_STATE publishes the confirmed state) and a different operation may even start
      // re-entrantly; an unchanged generation rules out both (see arm_disarm_generation()).
      const uint32_t gen = this->parent_->arm_disarm_generation();
      if (this->parent_->arm_away(code) && this->parent_->arm_disarm_generation() == gen) {
        this->publish_state(alarm_control_panel::ACP_STATE_ARMING);
      }
      break;
    }
    case alarm_control_panel::ACP_STATE_ARMED_HOME: {
      std::string code = this->requires_code_to_arm_ ? this->resolve_code_(call) : "";
      if (this->requires_code_to_arm_ && code.empty()) {
        this->status_momentary_warning("Code required to arm", 2000);
        return;
      }
      const uint32_t gen = this->parent_->arm_disarm_generation();
      if (this->parent_->arm_stay(code) && this->parent_->arm_disarm_generation() == gen) {
        this->publish_state(alarm_control_panel::ACP_STATE_ARMING);
      }
      break;
    }
    case alarm_control_panel::ACP_STATE_DISARMED: {
      std::string code = this->resolve_code_(call);
      if (code.empty()) {
        this->status_momentary_warning("Code required to disarm", 2000);
        return;
      }
      const uint32_t gen = this->parent_->arm_disarm_generation();
      if (this->parent_->disarm(code) && this->parent_->arm_disarm_generation() == gen) {
        this->publish_state(alarm_control_panel::ACP_STATE_DISARMING);
      }
      break;
    }
    case alarm_control_panel::ACP_STATE_TRIGGERED:
    case alarm_control_panel::ACP_STATE_PENDING:
      this->publish_state(*target_state);
      break;
    default:
      ESP_LOGW(TAG_ACP, "Unsupported alarm control panel request");
      break;
  }
}
}  // namespace crow_alarm_panel
}  // namespace esphome
