#include "alarm_cloud_sync.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <cstring>

#define TAG "AlarmCloudSync"

AlarmCloudSync& AlarmCloudSync::GetInstance() {
    static AlarmCloudSync instance;
    return instance;
}

AlarmCloudSync::AlarmCloudSync() {}

AlarmCloudSync::~AlarmCloudSync() {
    StopAutoSync();
}

void AlarmCloudSync::Initialize(const std::string& server_url, const std::string& device_id, const std::string& token) {
    server_url_ = server_url;
    device_id_ = device_id;
    token_ = token;
    
    if (!server_url_.empty() && server_url_.back() == '/') {
        server_url_.pop_back();
    }
    
    ESP_LOGI(TAG, "Initialized with server: %s, device: %s", server_url_.c_str(), device_id_.c_str());
}

void AlarmCloudSync::SetSyncInterval(int seconds) {
    sync_interval_ = seconds > 0 ? seconds : 300;
}

void AlarmCloudSync::StartAutoSync() {
    if (sync_timer_ != nullptr) {
        return;
    }
    
    esp_timer_create_args_t timer_args = {
        .callback = SyncTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_sync_timer"
    };
    
    esp_timer_create(&timer_args, &sync_timer_);
    esp_timer_start_periodic(sync_timer_, sync_interval_ * 1000000LL);
    
    ESP_LOGI(TAG, "Auto sync started, interval: %d seconds", sync_interval_);
}

void AlarmCloudSync::StopAutoSync() {
    if (sync_timer_ != nullptr) {
        esp_timer_stop(sync_timer_);
        esp_timer_delete(sync_timer_);
        sync_timer_ = nullptr;
        ESP_LOGI(TAG, "Auto sync stopped");
    }
}

void AlarmCloudSync::SyncTimerCallback(void* arg) {
    AlarmCloudSync* sync = static_cast<AlarmCloudSync*>(arg);
    sync->FullSync();
}

void AlarmCloudSync::SyncToCloud(SyncCallback callback) {
    ESP_LOGI(TAG, "SyncToCloud called, is_syncing=%d, url=%s, device_id=%s", 
             is_syncing_, server_url_.c_str(), device_id_.c_str());
    
    if (is_syncing_ || server_url_.empty() || device_id_.empty()) {
        ESP_LOGW(TAG, "Sync not ready: is_syncing=%d, url_empty=%d, device_empty=%d",
                 is_syncing_, server_url_.empty(), device_id_.empty());
        if (callback) callback(false, "Sync not ready");
        return;
    }
    
    is_syncing_ = true;
    ESP_LOGI(TAG, "Starting sync to cloud...");
    
    auto& manager = AlarmManager::GetInstance();
    auto alarms = manager.GetAllAlarms();
    
    ESP_LOGI(TAG, "Found %d alarms to sync", (int)alarms.size());
    
    if (alarms.empty()) {
        ESP_LOGI(TAG, "No alarms to sync, skipping upload");
        is_syncing_ = false;
        if (callback) callback(true, "No alarms to sync");
        return;
    }
    
    // 构建批量上报 JSON
    cJSON* root = cJSON_CreateObject();
    cJSON* alarms_array = cJSON_CreateArray();
    
    for (const auto& alarm : alarms) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", alarm.id);
        cJSON_AddStringToObject(item, "name", alarm.name.c_str());
        cJSON_AddNumberToObject(item, "trigger_time", alarm.trigger_time);
        cJSON_AddNumberToObject(item, "hour", alarm.hour);
        cJSON_AddNumberToObject(item, "minute", alarm.minute);
        cJSON_AddNumberToObject(item, "repeat_count", alarm.repeat_count);
        cJSON_AddNumberToObject(item, "interval", alarm.interval);
        cJSON_AddBoolToObject(item, "enabled", alarm.enabled);
        cJSON_AddNumberToObject(item, "type", alarm.type);
        cJSON_AddItemToArray(alarms_array, item);
    }
    
    cJSON_AddItemToObject(root, "alarms", alarms_array);
    
    char* json_str = cJSON_PrintUnformatted(root);
    std::string body(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    ESP_LOGI(TAG, "Sending request body: %s", body.c_str());
    
    std::string response;
    bool success = SendRequest("POST", "/api/devices/" + device_id_ + "/alarms/report", body, response);
    
    ESP_LOGI(TAG, "SendRequest returned: %d, response: %s", success, response.c_str());
    
    last_sync_time_ = time(nullptr);
    is_syncing_ = false;
    
    std::string message = success ? "Synced " + std::to_string(alarms.size()) + " alarms" : "Sync failed";
    ESP_LOGI(TAG, "%s", message.c_str());
    
    if (callback) callback(success, message);
}

void AlarmCloudSync::SyncFromCloud(SyncCallback callback) {
    if (is_syncing_ || server_url_.empty() || device_id_.empty()) {
        if (callback) callback(false, "Sync not ready");
        return;
    }
    
    is_syncing_ = true;
    ESP_LOGI(TAG, "Starting sync from cloud...");
    
    std::vector<CloudAlarm> cloud_alarms;
    if (!DownloadAlarms(cloud_alarms)) {
        is_syncing_ = false;
        if (callback) callback(false, "Failed to download alarms");
        return;
    }
    
    auto& manager = AlarmManager::GetInstance();
    manager.ClearAllAlarms();
    
    for (const auto& cloud_alarm : cloud_alarms) {
        int hour = cloud_alarm.hour >= 0 ? cloud_alarm.hour : -1;
        int minute = cloud_alarm.minute >= 0 ? cloud_alarm.minute : -1;
        
        manager.AddAlarm(cloud_alarm.name, 0, hour, minute, 
                         cloud_alarm.repeat_count, cloud_alarm.interval);
    }
    
    last_sync_time_ = time(nullptr);
    is_syncing_ = false;
    
    std::string message = "Downloaded " + std::to_string(cloud_alarms.size()) + " alarms";
    ESP_LOGI(TAG, "%s", message.c_str());
    
    if (callback) callback(true, message);
}

void AlarmCloudSync::FullSync(SyncCallback callback) {
    SyncToCloud([this, callback](bool success, const std::string& msg) {
        if (success) {
            SyncFromCloud(callback);
        } else if (callback) {
            callback(false, msg);
        }
    });
}

bool AlarmCloudSync::SendRequest(const std::string& method, const std::string& path,
                                  const std::string& body, std::string& response) {
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(10);
    
    std::string url = server_url_ + path;
    
    if (!token_.empty()) {
        std::string auth = token_;
        if (auth.find(" ") == std::string::npos) {
            auth = "Bearer " + auth;
        }
        http->SetHeader("Authorization", auth.c_str());
    }
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Device-Id", device_id_.c_str());
    
    if (!http->Open(method.c_str(), url)) {
        ESP_LOGE(TAG, "Failed to connect to %s", url.c_str());
        return false;
    }
    
    if (!body.empty()) {
        http->Write(body.c_str(), body.size());
    }
    http->Write("", 0);
    
    int status = http->GetStatusCode();
    if (status != 200 && status != 201) {
        ESP_LOGE(TAG, "HTTP error: %d, url: %s", status, url.c_str());
        std::string resp = http->ReadAll();
        if (!resp.empty()) {
            ESP_LOGE(TAG, "Response: %s", resp.c_str());
        }
        http->Close();
        return false;
    }
    
    response = http->ReadAll();
    http->Close();
    
    ESP_LOGI(TAG, "HTTP success: %s", url.c_str());
    return true;
}

bool AlarmCloudSync::UploadAlarm(const Alarm& alarm, uint32_t& cloud_id) {
    std::string body = BuildAlarmJson(alarm);
    std::string response;
    
    if (!SendRequest("POST", "/api/devices/" + device_id_ + "/alarms", body, response)) {
        return false;
    }
    
    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) return false;
    
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (data) {
        cJSON* id = cJSON_GetObjectItem(data, "id");
        if (id && cJSON_IsNumber(id)) {
            cloud_id = (uint32_t)id->valueint;
        }
    }
    cJSON_Delete(root);
    
    return true;
}

bool AlarmCloudSync::UpdateCloudAlarm(uint32_t cloud_id, const Alarm& alarm) {
    std::string body = BuildAlarmJson(alarm);
    std::string response;
    
    return SendRequest("PUT", "/api/devices/" + device_id_ + "/alarms/" + std::to_string(cloud_id), 
                       body, response);
}

bool AlarmCloudSync::DeleteCloudAlarm(uint32_t cloud_id) {
    std::string response;
    return SendRequest("DELETE", "/api/devices/" + device_id_ + "/alarms/" + std::to_string(cloud_id), 
                       "", response);
}

bool AlarmCloudSync::DownloadAlarms(std::vector<CloudAlarm>& alarms) {
    std::string response;
    
    if (!SendRequest("GET", "/api/devices/" + device_id_ + "/alarms", "", response)) {
        return false;
    }
    
    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) return false;
    
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!data || !cJSON_IsArray(data)) {
        cJSON_Delete(root);
        return false;
    }
    
    int count = cJSON_GetArraySize(data);
    for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(data, i);
        alarms.push_back(ParseCloudAlarm(item));
    }
    
    cJSON_Delete(root);
    return true;
}

bool AlarmCloudSync::ClearCloudAlarms() {
    std::string response;
    return SendRequest("DELETE", "/api/devices/" + device_id_ + "/alarms", "", response);
}

std::string AlarmCloudSync::BuildAlarmJson(const Alarm& alarm) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", alarm.name.c_str());
    
    if (alarm.hour >= 0 && alarm.minute >= 0) {
        cJSON_AddNumberToObject(root, "hour", alarm.hour);
        cJSON_AddNumberToObject(root, "minute", alarm.minute);
    } else {
        cJSON_AddNumberToObject(root, "delay", 60);
    }
    
    cJSON_AddNumberToObject(root, "repeat", alarm.repeat_count);
    cJSON_AddNumberToObject(root, "interval", alarm.interval);
    cJSON_AddBoolToObject(root, "enabled", alarm.enabled);
    
    char* json_str = cJSON_PrintUnformatted(root);
    std::string result(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    return result;
}

CloudAlarm AlarmCloudSync::ParseCloudAlarm(const cJSON* json) {
    CloudAlarm alarm;
    memset(&alarm, 0, sizeof(alarm));
    
    cJSON* item;
    
    item = cJSON_GetObjectItem(json, "id");
    if (item) alarm.cloud_id = (uint32_t)item->valueint;
    
    item = cJSON_GetObjectItem(json, "name");
    if (item && cJSON_IsString(item)) alarm.name = item->valuestring;
    
    item = cJSON_GetObjectItem(json, "trigger_time");
    if (item) alarm.trigger_time = (time_t)item->valuedouble;
    
    item = cJSON_GetObjectItem(json, "hour");
    if (item) alarm.hour = (int16_t)item->valueint;
    
    item = cJSON_GetObjectItem(json, "minute");
    if (item) alarm.minute = (int16_t)item->valueint;
    
    item = cJSON_GetObjectItem(json, "repeat_count");
    if (item) alarm.repeat_count = item->valueint;
    
    item = cJSON_GetObjectItem(json, "interval");
    if (item) alarm.interval = item->valueint;
    
    item = cJSON_GetObjectItem(json, "enabled");
    if (item) alarm.enabled = cJSON_IsTrue(item);
    
    item = cJSON_GetObjectItem(json, "type");
    if (item) alarm.type = (uint8_t)item->valueint;
    
    return alarm;
}