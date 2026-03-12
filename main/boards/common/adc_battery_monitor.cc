#include "adc_battery_monitor.h"
#include "esp_log.h"

AdcBatteryMonitor::AdcBatteryMonitor(adc_unit_t adc_unit, adc_channel_t adc_channel, float upper_resistor, float lower_resistor, gpio_num_t charging_pin)
    : charging_pin_(charging_pin) {
    
    // Initialize charging pin (only if it's not NC)
    if (charging_pin_ != GPIO_NUM_NC) {
        gpio_config_t gpio_cfg = {
            .pin_bit_mask = 1ULL << charging_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&gpio_cfg));
    }

    // Initialize ADC battery estimation
    adc_battery_estimation_t adc_cfg = {
        .internal = {
            .adc_unit = adc_unit,
            .adc_bitwidth = ADC_BITWIDTH_DEFAULT,
            .adc_atten = ADC_ATTEN_DB_12,
        },
        .adc_channel = adc_channel,
        .upper_resistor = upper_resistor,
        .lower_resistor = lower_resistor
    };

    // 在ADC配置部分进行条件设置
    if (charging_pin_ != GPIO_NUM_NC) {
        adc_cfg.charging_detect_cb = [](void *user_data) -> bool {
            AdcBatteryMonitor *self = (AdcBatteryMonitor *)user_data;
            return gpio_get_level(self->charging_pin_) == 1;
        };
        adc_cfg.charging_detect_user_data = this;
    } else {
        // 不设置回调，让adc_battery_estimation库使用软件估算
        adc_cfg.charging_detect_cb = nullptr;
        adc_cfg.charging_detect_user_data = nullptr;
    }
    
    ESP_LOGI("AdcBatteryMonitor", "Initializing: ADC_UNIT=%d, ADC_CHANNEL=%d, R1=%.0fΩ, R2=%.0fΩ, ChargingPin=%d", 
             adc_unit, adc_channel, upper_resistor, lower_resistor, charging_pin);
    
    adc_battery_estimation_handle_ = adc_battery_estimation_create(&adc_cfg);
    
    if (adc_battery_estimation_handle_ == nullptr) {
        ESP_LOGE("AdcBatteryMonitor", "Failed to create adc_battery_estimation_handle");
    } else {
        ESP_LOGI("AdcBatteryMonitor", "adc_battery_estimation_handle created successfully");
    }

    // Initialize timer
    esp_timer_create_args_t timer_cfg = {
        .callback = [](void *arg) {
            AdcBatteryMonitor *self = (AdcBatteryMonitor *)arg;
            self->CheckBatteryStatus();
        },
        .arg = this,
        .name = "adc_battery_monitor",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_cfg, &timer_handle_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1000000));
}

AdcBatteryMonitor::~AdcBatteryMonitor() {
    if (adc_battery_estimation_handle_) {
        ESP_ERROR_CHECK(adc_battery_estimation_destroy(adc_battery_estimation_handle_));
    }
    
    if (timer_handle_) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
    }
}

bool AdcBatteryMonitor::IsCharging() {
    // 优先使用adc_battery_estimation库的功能
    if (adc_battery_estimation_handle_ != nullptr) {
        bool is_charging = false;
        esp_err_t err = adc_battery_estimation_get_charging_state(adc_battery_estimation_handle_, &is_charging);
        if (err == ESP_OK) {
            ESP_LOGI("AdcBatteryMonitor", "IsCharging: adc_battery_estimation_get_charging_state returned is_charging=%s", is_charging ? "true" : "false");
            return is_charging;
        } else {
            ESP_LOGW("AdcBatteryMonitor", "IsCharging: adc_battery_estimation_get_charging_state failed, err=%d", err);
        }
    } else {
        ESP_LOGW("AdcBatteryMonitor", "IsCharging: handle is null");
    }
    
    // 回退到GPIO读取或返回默认值
    if (charging_pin_ != GPIO_NUM_NC) {
        int level = gpio_get_level(charging_pin_);
        bool charging = (level == 1);
        ESP_LOGI("AdcBatteryMonitor", "IsCharging: GPIO fallback, pin_level=%d, is_charging=%s", level, charging ? "true" : "false");
        return charging;
    }
    
    ESP_LOGI("AdcBatteryMonitor", "IsCharging: no charging pin configured, returning false");
    return false;
}

bool AdcBatteryMonitor::IsDischarging() {
    return !IsCharging();
}

uint8_t AdcBatteryMonitor::GetBatteryLevel() {
    // 如果句柄无效，返回默认值
    if (adc_battery_estimation_handle_ == nullptr) {
        ESP_LOGW("AdcBatteryMonitor", "GetBatteryLevel: handle is null, returning default 100%%");
        return 100;
    }
    
    float capacity = 0;
    esp_err_t err = adc_battery_estimation_get_capacity(adc_battery_estimation_handle_, &capacity);
    if (err != ESP_OK) {
        ESP_LOGE("AdcBatteryMonitor", "GetBatteryLevel: adc_battery_estimation_get_capacity failed, err=%d, returning default 100%%", err);
        return 100; // 出错时返回默认值
    }
    
    uint8_t level = (uint8_t)capacity;
    ESP_LOGI("AdcBatteryMonitor", "GetBatteryLevel: capacity=%.2f%%, returning level=%u%%", capacity, level);
    return level;
}

void AdcBatteryMonitor::OnChargingStatusChanged(std::function<void(bool)> callback) {
    on_charging_status_changed_ = callback;
}

void AdcBatteryMonitor::CheckBatteryStatus() {
    bool new_charging_status = IsCharging();
    uint8_t battery_level = GetBatteryLevel();
    
    // Log battery status every 5 seconds
    static int counter = 0;
    if (counter++ % 5 == 0) {
        ESP_LOGI("AdcBatteryMonitor", "Battery status: Level=%u%%, Charging=%s", 
                 battery_level, new_charging_status ? "YES" : "NO");
    }
    
    if (new_charging_status != is_charging_) {
        is_charging_ = new_charging_status;
        ESP_LOGI("AdcBatteryMonitor", "Charging status changed: %s", 
                 is_charging_ ? "CHARGING" : "NOT CHARGING");
        if (on_charging_status_changed_) {
            on_charging_status_changed_(is_charging_);
        }
    }
}