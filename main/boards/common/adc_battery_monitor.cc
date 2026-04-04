#include "adc_battery_monitor.h"
#include "esp_log.h"

static const char* TAG = "AdcBatteryMonitor";

AdcBatteryMonitor::AdcBatteryMonitor(adc_unit_t adc_unit, adc_channel_t adc_channel, float upper_resistor, float lower_resistor, gpio_num_t charging_pin, int charging_active_level, const battery_point_t* battery_points, size_t battery_points_count)
    : charging_pin_(charging_pin), charging_active_level_(charging_active_level), adc_unit_(adc_unit), adc_channel_(adc_channel), upper_resistor_(upper_resistor), lower_resistor_(lower_resistor) {
    
    // Initialize charging pin (only if it's not NC)
    if (charging_pin_ != GPIO_NUM_NC) {
        // 充电检测引脚需要上拉电阻，因为充电芯片CHRG引脚是开漏输出
        gpio_config_t gpio_cfg = {
            .pin_bit_mask = 1ULL << charging_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,   // 启用上拉电阻
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&gpio_cfg));
        ESP_LOGI(TAG, "Charging detection pin GPIO%d initialized with pull-up", charging_pin);
    }

    // Calculate voltage divider ratio
    voltage_divider_ratio_ = lower_resistor_ / (upper_resistor_ + lower_resistor_);
    ESP_LOGI(TAG, "Voltage divider ratio: %.4f (R1=%.0fΩ, R2=%.0fΩ)", voltage_divider_ratio_, upper_resistor_, lower_resistor_);

    // Initialize our own ADC handle
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = adc_unit,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_cfg, &adc_handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ADC unit: %s", esp_err_to_name(ret));
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_oneshot_config_channel(adc_handle_, adc_channel_, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC channel: %s", esp_err_to_name(ret));
        return;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = adc_unit,
        .chan = adc_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle_);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = adc_unit,
        .chan = adc_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_line_fitting(&cali_config, &adc_cali_handle_);
#endif
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ADC calibration: %s", esp_err_to_name(ret));
        return;
    }

    // Initialize ADC battery estimation using EXTERNAL handle mode
    adc_battery_estimation_t adc_cfg = {};
    adc_cfg.external.adc_handle = adc_handle_;
    adc_cfg.external.adc_cali_handle = adc_cali_handle_;
    adc_cfg.adc_channel = adc_channel;
    adc_cfg.upper_resistor = upper_resistor;
    adc_cfg.lower_resistor = lower_resistor;
    adc_cfg.battery_points = battery_points;
    adc_cfg.battery_points_count = battery_points_count;

    // 在ADC配置部分进行条件设置
    if (charging_pin_ != GPIO_NUM_NC) {
        adc_cfg.charging_detect_cb = [](void *user_data) -> bool {
            AdcBatteryMonitor *self = (AdcBatteryMonitor *)user_data;
            return gpio_get_level(self->charging_pin_) == self->charging_active_level_;
        };
        adc_cfg.charging_detect_user_data = this;
    } else {
        adc_cfg.charging_detect_cb = nullptr;
        adc_cfg.charging_detect_user_data = nullptr;
    }
    
    ESP_LOGI(TAG, "Initializing: ADC_UNIT=%d, ADC_CHANNEL=%d, R1=%.0fΩ, R2=%.0fΩ, ChargingPin=%d, ActiveLevel=%d, BatteryPoints=%d", 
             adc_unit, adc_channel, upper_resistor, lower_resistor, charging_pin, charging_active_level, battery_points_count);
    
    adc_battery_estimation_handle_ = adc_battery_estimation_create(&adc_cfg);
    
    if (adc_battery_estimation_handle_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create adc_battery_estimation_handle");
    } else {
        ESP_LOGI(TAG, "adc_battery_estimation_handle created successfully");
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
        adc_battery_estimation_destroy(adc_battery_estimation_handle_);
    }
    
    if (adc_cali_handle_) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_delete_scheme_curve_fitting(adc_cali_handle_);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_delete_scheme_line_fitting(adc_cali_handle_);
#endif
    }
    
    if (adc_handle_) {
        adc_oneshot_del_unit(adc_handle_);
    }
    
    if (timer_handle_) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
    }
}

bool AdcBatteryMonitor::IsCharging() {
    // 优先使用GPIO检测充电状态
    if (charging_pin_ != GPIO_NUM_NC) {
        int level = gpio_get_level(charging_pin_);
        bool charging = (level == charging_active_level_);
        ESP_LOGI(TAG, "IsCharging: GPIO%d level=%d, active_level=%d, charging=%s", 
                 charging_pin_, level, charging_active_level_, charging ? "YES" : "NO");
        return charging;
    }
    
    // 如果没有充电检测引脚，使用库的检测
    if (adc_battery_estimation_handle_ != nullptr) {
        bool is_charging = false;
        esp_err_t err = adc_battery_estimation_get_charging_state(adc_battery_estimation_handle_, &is_charging);
        if (err == ESP_OK) {
            return is_charging;
        }
    }
    
    return false;
}

bool AdcBatteryMonitor::IsDischarging() {
    return !IsCharging();
}

float AdcBatteryMonitor::GetBatteryVoltage() {
    if (adc_handle_ == nullptr || adc_cali_handle_ == nullptr) {
        return 0.0f;
    }
    
    // Read ADC multiple times and average (增加采样次数提高稳定性)
    int total_mv = 0;
    int valid_samples = 0;
    for (int i = 0; i < 10; i++) {  // 从5次增加到10次
        int adc_raw = 0;
        int voltage_mv = 0;
        esp_err_t ret = adc_oneshot_read(adc_handle_, adc_channel_, &adc_raw);
        if (ret != ESP_OK) {
            continue;
        }
        ret = adc_cali_raw_to_voltage(adc_cali_handle_, adc_raw, &voltage_mv);
        if (ret != ESP_OK) {
            continue;
        }
        total_mv += voltage_mv;
        valid_samples++;
    }
    
    if (valid_samples == 0) {
        return 0.0f;
    }
    
    float avg_mv = total_mv / (float)valid_samples;
    // Convert ADC voltage to battery voltage (account for voltage divider)
    float battery_voltage = avg_mv / 1000.0f / voltage_divider_ratio_;
    
    return battery_voltage;
}

uint8_t AdcBatteryMonitor::GetBatteryLevel() {
    if (adc_battery_estimation_handle_ == nullptr) {
        ESP_LOGW(TAG, "GetBatteryLevel: handle is null, returning default 100%%");
        return 100;
    }
    
    float capacity = 0;
    esp_err_t err = adc_battery_estimation_get_capacity(adc_battery_estimation_handle_, &capacity);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GetBatteryLevel: adc_battery_estimation_get_capacity failed, err=%d", err);
        return 100;
    }
    
    // 在转换前检查边界值
    if (capacity < 0) {
        return 0;
    }
    if (capacity > 100) {
        return 100;
    }
    
    return (uint8_t)capacity;
}

void AdcBatteryMonitor::OnChargingStatusChanged(std::function<void(bool)> callback) {
    on_charging_status_changed_ = callback;
}

void AdcBatteryMonitor::CheckBatteryStatus() {
    bool new_charging_status = IsCharging();
    uint8_t battery_level = GetBatteryLevel();
    float battery_voltage = GetBatteryVoltage();
    
    // Log battery status every second with voltage
    ESP_LOGI(TAG, "Battery: %.2fV, Level=%u%%, Charging=%s", 
             battery_voltage, battery_level, new_charging_status ? "YES" : "NO");
    
    if (new_charging_status != is_charging_) {
        is_charging_ = new_charging_status;
        ESP_LOGI(TAG, "Charging status changed: %s", 
                 is_charging_ ? "CHARGING" : "NOT CHARGING");
        if (on_charging_status_changed_) {
            on_charging_status_changed_(is_charging_);
        }
    }
}
