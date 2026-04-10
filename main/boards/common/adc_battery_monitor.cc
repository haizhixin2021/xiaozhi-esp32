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
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 5000000));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    err = nvs_open("battery", NVS_READWRITE, &nvs_handle_);
    if (err == ESP_OK) {
        nvs_ready_ = true;
    }

    if (nvs_ready_) {
        uint8_t saved_level = 0;

        if (nvs_get_u8(nvs_handle_, "level", &saved_level) == ESP_OK) {
            last_saved_level = saved_level;   // ⭐关键修复
        }
    }

    // ===== 读取校准参数 =====
    if (nvs_ready_) {
        size_t size = sizeof(float);

        if (nvs_get_blob(nvs_handle_, "cal_k", &k_, &size) == ESP_OK &&
            nvs_get_blob(nvs_handle_, "cal_b", &b_, &size) == ESP_OK) {

            ESP_LOGI("CAL", "Loaded calibration: k=%.4f b=%.4f", k_, b_);
        } else {
            k_ = 1.0f;
            b_ = 0.0f;
            ESP_LOGI("CAL", "No calibration data, use default");
        }
    }

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

    if (nvs_ready_) {
        nvs_close(nvs_handle_);
    }
}

bool AdcBatteryMonitor::IsCharging() {
    return is_charging_;
}

bool AdcBatteryMonitor::IsDischarging() {
    return !is_charging_;
}

float AdcBatteryMonitor::GetBatteryVoltage() {
    if (adc_handle_ == nullptr || adc_cali_handle_ == nullptr) {
        return 0.0f;
    }
    
    int total_mv = 0;
    int valid_samples = 0;
    for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
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

        vTaskDelay(2 / portTICK_PERIOD_MS);
    }
    
    if (valid_samples == 0) {
        return 0.0f;
    }
    
    float avg_mv = total_mv / (float)valid_samples;
    float battery_voltage = avg_mv / 1000.0f / voltage_divider_ratio_;
    
    // ADC校准系数：补偿ADC读取电压与实际电压的偏差
    // 根据实际测量，ADC读取电压比万用表测量值低约3%
    /* if (battery_voltage > 4.0f) {
        battery_voltage *= 1.03f;
    } else if (battery_voltage > 3.6f) {
        battery_voltage *= 1.02f;
    } else {
        battery_voltage *= 1.01f;
    } */

    // ⭐ 自动校准后的电压
    battery_voltage = battery_voltage * k_ + b_;
    
    ESP_LOGD(TAG, "ADC voltage: %.3fV, divider ratio: %.4f, battery voltage: %.3fV", 
             avg_mv / 1000.0f, voltage_divider_ratio_, battery_voltage);
    
    return battery_voltage;
}

uint8_t AdcBatteryMonitor::GetBatteryLevel() {
    if (adc_battery_estimation_handle_ == nullptr) {
        return 100;
    }
    
    float capacity = 0;
    esp_err_t err = adc_battery_estimation_get_capacity(adc_battery_estimation_handle_, &capacity);
    if (err != ESP_OK) {
        return 100;
    }
    
    if (capacity < 0) {
        capacity = 0;
    }
    if (capacity > 100) {
        capacity = 100;
    }
    
    uint8_t current_level = (uint8_t)capacity;

    int diff = abs((int)current_level - (int)last_level_);

    if (!first_read_ && diff > 20) {
        current_level = last_level_;   // 丢弃
    } else if (diff > 10) {
        current_level = (last_level_ + current_level) / 2; // ⭐ 拉回一半
    }

    if (is_charging_) {
        if (current_level > last_level_) {
            current_level = last_level_ + 1;  // 每次最多+1%
        } else {
            current_level = last_level_;     // 不允许下降
        }
    } else {
        // 放电允许缓慢下降
        if (current_level < last_level_) {
            current_level = last_level_ - 1;
        }
    }
    
    // ===== 第一次读取：从NVS恢复 =====
    if (first_read_) {
        //last_level_ = current_level;
        if (nvs_ready_) {
            uint8_t saved_level = 0;

            if (nvs_get_u8(nvs_handle_, "level", &saved_level) == ESP_OK) {
                // ⭐ 当前计算值（来自电压）
                uint8_t measured_level = current_level;

                // ⭐ 判断是否异常（比如换电池）
                if (abs((int)measured_level - (int)saved_level) > 30) {
                    // ⚠️ 不可信，用当前电压结果
                    last_level_ = measured_level;
                    current_level = measured_level;

                    ESP_LOGW("BAT", "NVS level invalid, use measured: %d%% (saved=%d%%)", 
                            measured_level, saved_level);
                } else {
                    // ✅ 正常情况：用历史值（更稳定）
                    last_level_ = saved_level;
                    current_level = saved_level;

                    ESP_LOGI("BAT", "Restore battery level from NVS: %d%%", saved_level);
                }

                //关键修复2：初始化保存时间（避免刚开机就触发写入）
                last_save_time = esp_timer_get_time();

                ESP_LOGI("BAT", "Restore battery level from NVS: %d%%", saved_level);
            } else {
                last_level_ = current_level;
            }
        } else {
            last_level_ = current_level;
        }

        // 初始化平滑窗口（必须用 last_level_）
        for (int i = 0; i < kWindowCount; i++) {
            level_window_[i] = last_level_;
        }
        first_read_ = false;
        return last_level_;
    }
    
    return GetSmoothedLevel(current_level);
}

uint8_t AdcBatteryMonitor::GetSmoothedLevel(uint8_t current_level) {
    level_window_[window_index_] = current_level;
    window_index_ = (window_index_ + 1) % kWindowCount;
    
    if (!window_filled_ && window_index_ == 0) {
        window_filled_ = true;
    }
    
    int count = window_filled_ ? kWindowCount : window_index_;
    if (count == 0) {
        return current_level;
    }
    
    uint32_t sum = 0;
    for (int i = 0; i < count; i++) {
        sum += level_window_[i];
    }
    uint8_t avg_level = sum / count;
    
    float alpha = 0.3f;
    if (is_charging_) {
        alpha = 0.5f;
    } else {
        if (current_level < last_level_) {
            alpha = 0.2f;
        } else {
            alpha = 0.15f;
        }
    }
    
    uint8_t smoothed_level = (uint8_t)(alpha * avg_level + (1.0f - alpha) * last_level_);
    
    int diff = abs((int)smoothed_level - (int)last_level_);
    if (diff > 3) {
        smoothed_level = last_level_ + (smoothed_level > last_level_ ? 1 : -1);
    }
    
    last_level_ = smoothed_level;
    return smoothed_level;
}

bool AdcBatteryMonitor::IsVoltageStable(float voltage) {
    if (last_stable_voltage_ == 0.0f) {
        last_stable_voltage_ = voltage;
        voltage_stable_count_ = 1;
        return false;
    }
    
    float voltage_diff = abs(voltage - last_stable_voltage_);
    
    if (voltage_diff < 0.02f) {
        voltage_stable_count_++;
        if (voltage_stable_count_ >= kVoltageStableThreshold) {
            return true;
        }
    } else {
        voltage_stable_count_ = 0;
        last_stable_voltage_ = voltage;
    }
    
    return false;
}

void AdcBatteryMonitor::OnChargingStatusChanged(std::function<void(bool)> callback) {
    on_charging_status_changed_ = callback;
}

void AdcBatteryMonitor::OnLowBatteryStatusChanged(std::function<void(bool)> callback) {
    on_low_battery_status_changed_ = callback;
}

bool AdcBatteryMonitor::IsBatteryConnected() {
    static int valid_cnt = 0;

    if (last_voltage_ > 3.2f && last_voltage_ < 4.4f) {
        valid_cnt++;
    } else {
        valid_cnt = 0;
    }

    return valid_cnt >= 3;
}

void AdcBatteryMonitor::CheckBatteryStatus() {
    // ① 先预热（过滤垃圾数据）
    static int warmup_count = 0;
    if (warmup_count < 20) {
        GetBatteryVoltage();
        warmup_count++;
        return;
    }

    bool new_charging_status = false;
    
    if (charging_pin_ != GPIO_NUM_NC) {
        int level = gpio_get_level(charging_pin_);
        //new_charging_status = (level == charging_active_level_);

        static int charge_stable_count = 0;
        static bool last_raw_status = false;

        bool raw = (level == charging_active_level_);

        if (raw == last_raw_status) {
            charge_stable_count++;
        } else {
            charge_stable_count = 0;
        }

        last_raw_status = raw;
        static int64_t last_change_time = 0;
        int64_t now = esp_timer_get_time();

        if (charge_stable_count >= 3 && (now - last_change_time) > 1000000) {
            new_charging_status = raw;
            last_change_time = now;
        }


    } else if (adc_battery_estimation_handle_ != nullptr) {
        bool is_charging = false;
        esp_err_t err = adc_battery_estimation_get_charging_state(adc_battery_estimation_handle_, &is_charging);
        if (err == ESP_OK) {
            new_charging_status = is_charging;
        }
    }
    
    
    float battery_voltage = GetBatteryVoltage();   // 先读电压
    //只调用一次！
    bool voltage_stable = IsVoltageStable(battery_voltage);
    //在这里加入“电压锁定机制”
    static int startup_counter = 0;
    startup_counter++;
    // 启动阶段锁定
    if (!startup_stable_) {

        static int stable_count = 0;

        if (voltage_stable) {
            stable_count++;
            if (stable_count >= 3) {
                startup_stable_ = true;
            }
        } else {
            stable_count = 0;
        }

        // ⭐ 防死锁（5秒强制进入）
        if (startup_counter > 25 && battery_voltage > 3.0f) {
            startup_stable_ = true;
        }

        return;
    }

    // ================== 自动校准系统 ==================
    if (startup_stable_ && voltage_stable) {

        // ===== 满电采样（4.2V）=====
        static int full_stable_cnt = 0;

        if (is_charging_ && battery_voltage > 4.15f && battery_voltage < 4.25f) {
            full_stable_cnt++;

            // 始终记录最大值（关键优化）
            cal_v_adc_full_ = fmaxf(cal_v_adc_full_, battery_voltage);

            if (full_stable_cnt >= 8) {
                cal_has_full_ = true;

                ESP_LOGI("CAL", "Captured FULL point: adc=%.3f", battery_voltage);
                    full_stable_cnt = 0;
            }
        } else {
            full_stable_cnt = 0;
        }

        // ===== 中低电采样（3.5~3.7）=====
        static int low_stable_cnt = 0;

        if (!is_charging_ && battery_voltage > 3.5f && battery_voltage < 3.7f) {
            low_stable_cnt++;

            if (low_stable_cnt >= 5) {
                cal_v_adc_low_ = battery_voltage;
                cal_has_low_ = true;

                ESP_LOGI("CAL", "Captured LOW point: adc=%.3f", battery_voltage);
                    low_stable_cnt = 0;
            }
        } else {
            low_stable_cnt = 0;
        }

        // ===== 计算校准参数 =====
        if (cal_has_full_ && cal_has_low_) {

            float delta_adc = cal_v_adc_full_ - cal_v_adc_low_;

            // 防止除0
            if (delta_adc > 0.05f) {

                float new_k = (cal_v_real_full_ - cal_v_real_low_) / delta_adc;
                float new_b = cal_v_real_low_ - new_k * cal_v_adc_low_;

                // ⭐ 安全范围限制（非常重要）
                if (new_k > 0.9f && new_k < 1.2f && new_b > -0.5f && new_b < 0.5f) {

                    k_ = new_k;
                    b_ = new_b;

                    ESP_LOGI("CAL", "Updated: k=%.4f b=%.4f", k_, b_);

                    // ===== 写入NVS =====
                    if (nvs_ready_) {
                        nvs_set_blob(nvs_handle_, "cal_k", &k_, sizeof(float));
                        nvs_set_blob(nvs_handle_, "cal_b", &b_, sizeof(float));
                        nvs_commit(nvs_handle_);
                    }
                } else {
                    ESP_LOGW("CAL", "Invalid calibration result, ignored");
                }
            }

            // ⭐ 清标志（避免反复算）
            cal_has_full_ = false;
            cal_has_low_ = false;
            cal_v_adc_full_ = 0.0f;
            cal_v_adc_low_ = 0.0f;
        }
    }

    uint8_t battery_level = GetBatteryLevel();     // 后算电量
        
    int64_t now = esp_timer_get_time(); // 微秒

    bool big_change = abs((int)battery_level - (int)last_saved_level) >= 5;
    bool small_change = abs((int)battery_level - (int)last_saved_level) >= 2;
    bool low_battery = battery_voltage < 3.3f;

    bool time_1min = (now - last_save_time) > 60000000;
    bool time_5min = (now - last_save_time) > 300000000;

    bool need_save =  
                      (big_change && time_1min) ||       // ⭐ 立即写（重要变化）
                      (small_change && time_5min) ||       // ⭐ 限频写
                      (low_battery && time_5min);          // ⭐ 低电量保护但限频
                    
    // ⭐ 写入冷却保护
    static int64_t next_allowed_save_time = 0;

    if (now < next_allowed_save_time) {
        need_save = false;
    }

    if (need_save) {
        next_allowed_save_time = now + 15000000; // 15秒
    }

    if (!IsBatteryConnected() || !voltage_stable) {
        need_save = false;
    }

    // ⭐ 保存电量到NVS（带限频）
    if (nvs_ready_ && need_save) {
        nvs_set_u8(nvs_handle_, "level", battery_level);
        nvs_commit(nvs_handle_);
        
        ESP_LOGI(TAG, "Battery level saved to NVS: %d%% (voltage: %.2fV)", battery_level, battery_voltage);
        
        last_saved_level = battery_level;
        last_save_time = now;
    }
    
    last_voltage_ = battery_voltage;
        
    log_counter_++;
    bool should_log = false;
    
    if (last_logged_level_ == 255) {
        should_log = true;
    } else if (abs((int)battery_level - (int)last_logged_level_) >= 5) {
        should_log = true;
    } else if (log_counter_ >= kLogInterval) {
        should_log = true;
        log_counter_ = 0;
    }
    
    if (should_log) {
        ESP_LOGI(TAG, "Battery: %.2fV, Level=%u%%, Charging=%s, Stable=%s", 
                 battery_voltage, battery_level, 
                 new_charging_status ? "YES" : "NO",
                 voltage_stable ? "YES" : "NO");
        last_logged_level_ = battery_level;
    }
    
    if (new_charging_status != is_charging_) {
        is_charging_ = new_charging_status;
        ESP_LOGI(TAG, "Charging status changed: %s", 
                 is_charging_ ? "CHARGING" : "NOT CHARGING");
        if (on_charging_status_changed_) {
            on_charging_status_changed_(is_charging_);
        }
    }
    
    bool new_low_battery_status = (battery_level <= 20);
    if (new_low_battery_status != is_low_battery_) {
        is_low_battery_ = new_low_battery_status;
        ESP_LOGI(TAG, "Low battery status changed: %s", 
                 is_low_battery_ ? "LOW" : "NORMAL");
        if (on_low_battery_status_changed_) {
            on_low_battery_status_changed_(is_low_battery_);
        }
    }
}
