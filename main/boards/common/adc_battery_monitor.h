#ifndef ADC_BATTERY_MONITOR_H
#define ADC_BATTERY_MONITOR_H
#define ADC_SAMPLE_COUNT 8

#include <functional>
#include <driver/gpio.h>
#include <adc_battery_estimation.h>
#include <esp_timer.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>

#include "nvs.h"
#include "nvs_flash.h"

// ===== 快速测试模式（验证后改为0关闭） =====
#define CALIBRATION_TEST_MODE 1

#if CALIBRATION_TEST_MODE
#define CAL_SAMPLE_COUNT 3
#define CAL_FULL_MIN 4.0f
#define CAL_FULL_MAX 4.3f
#define CAL_LOW_MIN 3.4f
#define CAL_LOW_MAX 3.8f
#define CAL_CAN_SAMPLE true
#else
#define CAL_SAMPLE_COUNT 8
#define CAL_FULL_MIN 4.10f
#define CAL_FULL_MAX 4.20f
#define CAL_LOW_MIN 3.5f
#define CAL_LOW_MAX 3.7f
#define CAL_CAN_SAMPLE (!is_charging_)
#endif



class AdcBatteryMonitor {
public:
    AdcBatteryMonitor(adc_unit_t adc_unit, adc_channel_t adc_channel, float upper_resistor, float lower_resistor, gpio_num_t charging_pin = GPIO_NUM_NC, int charging_active_level = 1, const battery_point_t* battery_points = nullptr, size_t battery_points_count = 0);
    ~AdcBatteryMonitor();

    bool IsCharging();
    bool IsDischarging();
    uint8_t GetBatteryLevel();
    float GetBatteryVoltage();
    bool IsBatteryConnected();

    void OnChargingStatusChanged(std::function<void(bool)> callback);
    void OnLowBatteryStatusChanged(std::function<void(bool)> callback);

private:
    gpio_num_t charging_pin_;
    int charging_active_level_;
    adc_unit_t adc_unit_;
    adc_channel_t adc_channel_;
    float upper_resistor_;
    float lower_resistor_;
    float voltage_divider_ratio_;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_cali_handle_t adc_cali_handle_ = nullptr;
    adc_battery_estimation_handle_t adc_battery_estimation_handle_ = nullptr;
    esp_timer_handle_t timer_handle_ = nullptr;
    bool is_charging_ = false;
    float last_voltage_ = 0.0f;
    uint8_t last_level_ = 0;
    bool first_read_ = true;
    std::function<void(bool)> on_charging_status_changed_;
    std::function<void(bool)> on_low_battery_status_changed_;

    nvs_handle_t nvs_handle_;
    bool nvs_ready_ = false;
    
    bool is_low_battery_ = false;
    uint8_t last_logged_level_ = 255;
    int log_counter_ = 0;
    static const int kLogInterval = 12;
    
    static const int kWindowCount = 5;
    uint8_t level_window_[5] = {0};
    int window_index_ = 0;
    bool window_filled_ = false;
    float last_stable_voltage_ = 0.0f;
    int voltage_stable_count_ = 0;
    static const int kVoltageStableThreshold = 3;
    bool startup_stable_ = false;
    uint8_t last_saved_level = 255;
    int64_t last_save_time = 0;

    // ===== 自动校准参数 =====
    float k_ = 1.01f;
    float b_ = 0.0f;

    // ===== 校准采样点 =====
    float cal_v_adc_full_ = 0.0f;
    float cal_v_adc_low_ = 0.0f;

    float cal_v_real_full_ = 4.2f;
    float cal_v_real_low_ = 3.65f;   // 推荐固定（更稳定）

    bool cal_has_full_ = false;
    bool cal_has_low_ = false;
    bool init_ok_ = false;
    
    // ===== 电池映射表 =====
    const battery_point_t* battery_points_ = nullptr;
    size_t battery_points_count_ = 0;

    void CheckBatteryStatus();
    uint8_t GetSmoothedLevel(uint8_t current_level);
    bool IsVoltageStable(float voltage);
    uint8_t CalculateBatteryLevel(float voltage);  // 新增：自己计算百分比
};

#endif // ADC_BATTERY_MONITOR_H
