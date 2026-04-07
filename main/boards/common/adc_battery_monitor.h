#ifndef ADC_BATTERY_MONITOR_H
#define ADC_BATTERY_MONITOR_H

#include <functional>
#include <driver/gpio.h>
#include <adc_battery_estimation.h>
#include <esp_timer.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

class AdcBatteryMonitor {
public:
    AdcBatteryMonitor(adc_unit_t adc_unit, adc_channel_t adc_channel, float upper_resistor, float lower_resistor, gpio_num_t charging_pin = GPIO_NUM_NC, int charging_active_level = 1, const battery_point_t* battery_points = nullptr, size_t battery_points_count = 0);
    ~AdcBatteryMonitor();

    bool IsCharging();
    bool IsDischarging();
    uint8_t GetBatteryLevel();
    float GetBatteryVoltage();
    bool IsBatteryConnected();  // 检测电池是否连接

    void OnChargingStatusChanged(std::function<void(bool)> callback);

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
    float last_voltage_ = 0.0f;  // 记录上次电压
    uint8_t last_level_ = 0;     // 记录上次电量，用于平滑滤波
    bool first_read_ = true;     // 首次读取标志
    std::function<void(bool)> on_charging_status_changed_;

    void CheckBatteryStatus();
};

#endif // ADC_BATTERY_MONITOR_H
