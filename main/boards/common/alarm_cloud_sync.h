#ifndef ALARM_CLOUD_SYNC_H
#define ALARM_CLOUD_SYNC_H

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cJSON.h>
#include "alarm_clock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

struct CloudAlarm {
    uint32_t cloud_id;
    uint32_t local_id;
    std::string name;
    time_t trigger_time;
    int16_t hour;
    int16_t minute;
    int32_t repeat_count;
    int32_t interval;
    bool enabled;
    uint8_t type;
    std::string sync_status;
};

class AlarmCloudSync {
public:
    using SyncCallback = std::function<void(bool success, const std::string& message)>;

    static AlarmCloudSync& GetInstance();

    void Initialize(const std::string& server_url, const std::string& device_id, const std::string& token = "");
    
    void SyncToCloud(SyncCallback callback = nullptr);
    void SyncFromCloud(SyncCallback callback = nullptr);
    void FullSync(SyncCallback callback = nullptr);
    
    void SetSyncInterval(int seconds);
    void StartAutoSync();
    void StopAutoSync();
    
    bool IsSyncing() const { return is_syncing_; }
    time_t GetLastSyncTime() const { return last_sync_time_; }

private:
    AlarmCloudSync();
    ~AlarmCloudSync();

    bool UploadAlarm(const Alarm& alarm, uint32_t& cloud_id);
    bool UpdateCloudAlarm(uint32_t cloud_id, const Alarm& alarm);
    bool DeleteCloudAlarm(uint32_t cloud_id);
    bool DownloadAlarms(std::vector<CloudAlarm>& alarms);
    bool ClearCloudAlarms();
    
    std::string BuildAlarmJson(const Alarm& alarm);
    CloudAlarm ParseCloudAlarm(const cJSON* json);
    bool IsAlarmChanged(const Alarm& local, const CloudAlarm& cloud);
    
    bool SendRequest(const std::string& method, const std::string& path, 
                     const std::string& body, std::string& response);
    
    void SyncTaskFunc();
    static void SyncTaskEntry(void* arg);
    static void SyncTimerCallback(void* arg);
    
    std::string server_url_;
    std::string device_id_;
    std::string token_;
    int sync_interval_ = 300;
    bool is_syncing_ = false;
    time_t last_sync_time_ = 0;
    esp_timer_handle_t sync_timer_ = nullptr;
    
    TaskHandle_t sync_task_ = nullptr;
    QueueHandle_t sync_queue_ = nullptr;
    bool stop_requested_ = false;
};

#endif