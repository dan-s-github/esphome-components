#pragma once

#include "esphome/components/button/button.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "../crow_alarm_panel.h"

namespace esphome {
namespace crow_alarm_panel {

class CrowAlarmPanelButton : public button::Button, public Component {
 public:
  void set_parent(CrowAlarmPanel *parent) { this->parent_ = parent; }
  void set_button_type(const std::string &type) { this->button_type_ = type; }
  void set_code(const std::string &code) { this->code_ = code; }

 protected:
  void press_action() override {
   if (this->parent_ == nullptr) {
     ESP_LOGE("crow_alarm_panel.button", "Parent not set, ignoring button press");
     return;
   }
   if (this->button_type_ == "arm_away") {
      this->parent_->arm_away(this->code_);
    } else if (this->button_type_ == "arm_stay") {
      this->parent_->arm_stay(this->code_);
    } else if (this->button_type_ == "disarm") {
      if (!this->parent_->is_armed()) {
        ESP_LOGW("crow_alarm_panel.button", "Cannot disarm - alarm is not armed");
        return;
      }
      this->parent_->disarm(this->code_);
    }
  }

  CrowAlarmPanel *parent_{nullptr};
  std::string button_type_;
  std::string code_;
};

}  // namespace crow_alarm_panel
}  // namespace esphome
