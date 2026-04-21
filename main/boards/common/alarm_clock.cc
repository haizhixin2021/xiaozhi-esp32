#include "alarm_clock.h"
#include "settings.h"

#include <esp_log.h>
#include <algorithm>
#include <cstring>
#include <cinttypes>

#define TAG "AlarmClock"

std::string Alarm::ToJson() const {
    cJSON* json = ToCjson();
    char* json_str = cJSON_PrintUnformatted(json);
    std::string result(json_str);
    cJSON_free(json_str);
    cJSON_Delete(json);
    return result;
}

Alarm Alarm::FromJson(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        return Alarm();
    }
    Alarm alarm = FromCjson(root);
    cJSON_Delete(root);
    return alarm;
}

cJSON* Alarm::ToCjson() const {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "id", id);
    cJSON_AddNumberToObject(json, "trigger_time", (double)trigger_time);
    cJSON_AddNumberToObject(json, "hour", hour);
    cJSON_AddNumberToObject(json, "minute", minute);
    cJSON_AddNumberToObject(json, "repeat_count", repeat_count);
    cJSON_AddNumberToObject(json, "interval", interval);
    cJSON_AddStringToObject(json, "name", name.c_str());
    cJSON_AddBoolToObject(json, "enabled", enabled);
    cJSON_AddNumberToObject(json, "type", type);
    return json;
}

Alarm Alarm::FromCjson(const cJSON* json) {
    Alarm alarm;
    if (!json) return alarm;

    cJSON* item;
    
    item = cJSON_GetObjectItem(json, "id");
    if (item && cJSON_IsNumber(item)) {
        alarm.id = (uint32_t)item->valueint;
    }
    
    item = cJSON_GetObjectItem(json, "trigger_time");
    if (item && cJSON_IsNumber(item)) {
        alarm.trigger_time = (time_t)item->valuedouble;
    }
    
    item = cJSON_GetObjectItem(json, "hour");
    if (item && cJSON_IsNumber(item)) {
        alarm.hour = (int16_t)item->valueint;
    }
    
    item = cJSON_GetObjectItem(json, "minute");
    if (item && cJSON_IsNumber(item)) {
        alarm.minute = (int16_t)item->valueint;
    }
    
    item = cJSON_GetObjectItem(json, "repeat_count");
    if (item && cJSON_IsNumber(item)) {
        alarm.repeat_count = item->valueint;
    }
    
    item = cJSON_GetObjectItem(json, "interval");
    if (item && cJSON_IsNumber(item)) {
        alarm.interval = item->valueint;
    }
    
    item = cJSON_GetObjectItem(json, "name");
    if (item && cJSON_IsString(item)) {
        alarm.name = item->valuestring;
    }
    
    item = cJSON_GetObjectItem(json, "enabled");
    if (item && cJSON_IsBool(item)) {
        alarm.enabled = cJSON_IsTrue(item);
    }
    
    item = cJSON_GetObjectItem(json, "type");
    if (item && cJSON_IsNumber(item)) {
        alarm.type = (uint8_t)item->valueint;
    }
    
    return alarm;
}

AlarmStorage::AlarmStorage() {
}

AlarmStorage::~AlarmStorage() {
}

bool AlarmStorage::LoadAll(std::vector<Alarm>& alarms) {
    Settings settings(kNamespace, false);
    
    int32_t count = settings.GetInt(kCountKey, 0);
    if (count <= 0) {
        return true;
    }
    
    alarms.clear();
    alarms.reserve(count);
    
    for (int32_t i = 0; i < count && (int32_t)alarms.size() < kMaxAlarms; i++) {
        char key[20];
        snprintf(key, sizeof(key), "alarm_%" PRId32, i);
        std::string json = settings.GetString(key, "");
        if (!json.empty()) {
            Alarm alarm = Alarm::FromJson(json);
            if (alarm.id > 0) {
                alarms.push_back(alarm);
            }
        }
    }
    
    return true;
}

bool AlarmStorage::SaveAlarm(const Alarm& alarm) {
    if (alarm.id == 0) {
        ESP_LOGW(TAG, "Cannot save alarm with id 0");
        return false;
    }
    
    Settings settings(kNamespace, true);
    
    int32_t count = settings.GetInt(kCountKey, 0);
    
    int32_t existing_index = -1;
    for (int32_t i = 0; i < count; i++) {
        char key[20];
        snprintf(key, sizeof(key), "alarm_%" PRId32, i);
        std::string json = settings.GetString(key, "");
        if (!json.empty()) {
            Alarm existing = Alarm::FromJson(json);
            if (existing.id == alarm.id) {
                existing_index = i;
                break;
            }
        }
    }
    
    std::string json = alarm.ToJson();
    
    ESP_LOGI(TAG, "NVS write: alarm id=%u, name=%s, json_len=%d", 
             alarm.id, alarm.name.c_str(), (int)json.length());
    
    if (existing_index >= 0) {
        char key[20];
        snprintf(key, sizeof(key), "alarm_%" PRId32, existing_index);
        settings.SetString(key, json);
        ESP_LOGI(TAG, "NVS update: key=%s (existing)", key);
    } else {
        if (count >= kMaxAlarms) {
            ESP_LOGW(TAG, "Max alarms reached (%d)", kMaxAlarms);
            return false;
        }
        char key[20];
        snprintf(key, sizeof(key), "alarm_%" PRId32, count);
        settings.SetString(key, json);
        settings.SetInt(kCountKey, count + 1);
        ESP_LOGI(TAG, "NVS add: key=%s, total_count=%d", key, count + 1);
    }
    
    ESP_LOGI(TAG, "Alarm saved: id=%u, name=%s", alarm.id, alarm.name.c_str());
    return true;
}

bool AlarmStorage::DeleteAlarm(uint32_t id) {
    Settings settings(kNamespace, true);
    
    int32_t count = settings.GetInt(kCountKey, 0);
    if (count <= 0) {
        return false;
    }
    
    std::vector<Alarm> remaining_alarms;
    for (int32_t i = 0; i < count; i++) {
        char key[20];
        snprintf(key, sizeof(key), "alarm_%" PRId32, i);
        std::string json = settings.GetString(key, "");
        if (!json.empty()) {
            Alarm alarm = Alarm::FromJson(json);
            if (alarm.id != id) {
                remaining_alarms.push_back(alarm);
            }
        }
    }
    
    settings.EraseAll();
    ESP_LOGI(TAG, "NVS erase all: clearing namespace for delete operation");
    
    for (size_t i = 0; i < remaining_alarms.size(); i++) {
        char key[20];
        snprintf(key, sizeof(key), "alarm_%d", (int)i);
        settings.SetString(key, remaining_alarms[i].ToJson());
    }
    settings.SetInt(kCountKey, remaining_alarms.size());
    ESP_LOGI(TAG, "NVS rewrite: %d remaining alarms after delete", (int)remaining_alarms.size());
    
    ESP_LOGI(TAG, "Alarm deleted: id=%u", id);
    return true;
}

bool AlarmStorage::ClearAll() {
    Settings settings(kNamespace, true);
    settings.EraseAll();
    ESP_LOGI(TAG, "NVS erase all: cleared all alarms from namespace");
    return true;
}

uint32_t AlarmStorage::GetNextId() {
    Settings settings(kNamespace, true);
    int32_t next_id = settings.GetInt(kNextIdKey, 1);
    settings.SetInt(kNextIdKey, next_id + 1);
    ESP_LOGI(TAG, "NVS write: next_id=%d (incremented)", next_id + 1);
    return (uint32_t)next_id;
}

AlarmScheduler::AlarmScheduler() {
}

AlarmScheduler::~AlarmScheduler() {
    Cancel();
}

void AlarmScheduler::SetCallback(AlarmCallback callback) {
    callback_ = callback;
}

void AlarmScheduler::ScheduleNext(const std::vector<Alarm>& alarms) {
    Cancel();
    
    if (alarms.empty() || !callback_) {
        return;
    }
    
    time_t now = time(nullptr);
    Alarm* next_alarm = nullptr;
    time_t next_time = 0;
    
    for (auto& alarm : alarms) {
        if (!alarm.enabled) {
            continue;
        }
        
        time_t alarm_time = alarm.trigger_time;
        
        if (alarm_time <= now) {
            continue;
        }
        
        if (next_alarm == nullptr || alarm_time < next_time) {
            next_alarm = const_cast<Alarm*>(&alarm);
            next_time = alarm_time;
        }
    }
    
    if (next_alarm == nullptr) {
        ESP_LOGI(TAG, "No future alarms to schedule");
        return;
    }
    
    int64_t delay_us = (next_time - now) * 1000000LL;
    
    pending_alarm_ = *next_alarm;
    
    esp_timer_create_args_t timer_args = {
        .callback = TimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_timer"
    };
    
    esp_err_t ret = esp_timer_create(&timer_args, &timer_handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        return;
    }
    
    ret = esp_timer_start_once(timer_handle_, delay_us);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer: %s", esp_err_to_name(ret));
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
        return;
    }
    
    char time_str[32];
    struct tm* tm_info = localtime(&next_time);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    ESP_LOGI(TAG, "Scheduled alarm id=%u, name=%s, trigger at %s", 
             pending_alarm_.id, pending_alarm_.name.c_str(), time_str);
}

void AlarmScheduler::Cancel() {
    if (timer_handle_) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
        ESP_LOGI(TAG, "Timer cancelled");
    }
    pending_alarm_ = Alarm();
}

void AlarmScheduler::TimerCallback(void* arg) {
    AlarmScheduler* scheduler = static_cast<AlarmScheduler*>(arg);
    scheduler->OnTimerTriggered();
}

void AlarmScheduler::OnTimerTriggered() {
    ESP_LOGI(TAG, "Alarm triggered: id=%u, name=%s", 
             pending_alarm_.id, pending_alarm_.name.c_str());
    
    if (callback_) {
        callback_(pending_alarm_);
    }
    
    if (timer_handle_) {
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
    }
}

AlarmManager& AlarmManager::GetInstance() {
    static AlarmManager instance;
    return instance;
}

AlarmManager::AlarmManager() {
    storage_ = std::make_unique<AlarmStorage>();
    scheduler_ = std::make_unique<AlarmScheduler>();
}

AlarmManager::~AlarmManager() {
}

void AlarmManager::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    storage_->LoadAll(alarms_);
    
    scheduler_->SetCallback([this](const Alarm& alarm) {
        OnAlarmTriggered(alarm);
    });
    
    Reschedule();
    
    ESP_LOGI(TAG, "Initialized with %d alarms", (int)alarms_.size());
}

void AlarmManager::SetAlarmCallback(AlarmCallback callback) {
    alarm_callback_ = callback;
}

void AlarmManager::SetAlarmChangeCallback(AlarmChangeCallback callback) {
    alarm_change_callback_ = callback;
}

void AlarmManager::NotifyAlarmChange(const Alarm& alarm, const std::string& action) {
    if (alarm_change_callback_) {
        alarm_change_callback_(alarm, action);
    }
}

Alarm AlarmManager::AddAlarm(const std::string& name, int delay, int hour, int minute,
                              int repeat, int interval) {
    Alarm alarm;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        alarm.id = storage_->GetNextId();
        alarm.name = name;
        alarm.enabled = true;
        alarm.repeat_count = repeat > 0 ? repeat : 1;
        alarm.interval = interval > 0 ? interval : 0;
        
        time_t now = time(nullptr);
        
        if (hour >= 0 && hour < 24 && minute >= 0 && minute < 60) {
            alarm.hour = (int16_t)hour;
            alarm.minute = (int16_t)minute;
            alarm.type = (repeat > 1 || repeat == -1) ? kAlarmTypeRepeating : kAlarmTypeScheduled;
            
            struct tm* tm_info = localtime(&now);
            tm_info->tm_hour = hour;
            tm_info->tm_min = minute;
            tm_info->tm_sec = 0;
            time_t target_time = mktime(tm_info);
            
            if (target_time <= now) {
                target_time += 86400;
            }
            
            alarm.trigger_time = target_time;
        } else {
            alarm.hour = -1;
            alarm.minute = -1;
            alarm.type = (repeat > 1 || repeat == -1) ? kAlarmTypeRepeating : kAlarmTypeCountdown;
            alarm.trigger_time = now + delay;
        }
        
        if (repeat == -1) {
            alarm.repeat_count = -1;
        }
        
        storage_->SaveAlarm(alarm);
        alarms_.push_back(alarm);
        
        Reschedule();
    }

    NotifyAlarmChange(alarm, "add");
    
    char time_str[32];
    struct tm* tm_info = localtime(&alarm.trigger_time);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    ESP_LOGI(TAG, "Added alarm id=%u, name=%s, trigger at %s, repeat=%d, interval=%d", 
             alarm.id, alarm.name.c_str(), time_str, alarm.repeat_count, alarm.interval);
    
    return alarm;
}

bool AlarmManager::RemoveAlarm(uint32_t id) {
    Alarm alarm;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = std::find_if(alarms_.begin(), alarms_.end(), 
                               [id](const Alarm& a) { return a.id == id; });
        
        if (it == alarms_.end()) {
            ESP_LOGW(TAG, "Alarm not found: id=%u", id);
            return false;
        }
        
        alarm = *it;

        storage_->DeleteAlarm(id);
        alarms_.erase(it);
        
        Reschedule();
    }

    NotifyAlarmChange(alarm, "delete");
    
    ESP_LOGI(TAG, "Removed alarm id=%u", id);
    return true;
}

bool AlarmManager::UpdateAlarm(uint32_t id, const std::string& name, int hour, int minute,
                                int repeat, int interval) {
    Alarm alarm;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = std::find_if(alarms_.begin(), alarms_.end(), 
                               [id](const Alarm& a) { return a.id == id; });
        
        if (it == alarms_.end()) {
            ESP_LOGW(TAG, "Alarm not found: id=%u", id);
            return false;
        }
        
        if (!name.empty()) {
            it->name = name;
        }
        
        time_t now = time(nullptr);
        
        if (hour >= 0 && hour < 24 && minute >= 0 && minute < 60) {
            it->hour = (int16_t)hour;
            it->minute = (int16_t)minute;
            
            struct tm* tm_info = localtime(&now);
            tm_info->tm_hour = hour;
            tm_info->tm_min = minute;
            tm_info->tm_sec = 0;
            time_t target_time = mktime(tm_info);
            
            if (target_time <= now) {
                target_time += 86400;
            }
            
            it->trigger_time = target_time;
        }
        
        if (repeat != 0) {
            it->repeat_count = repeat == -1 ? -1 : repeat;
        }
        
        if (interval > 0) {
            it->interval = interval;
        }
        
        if (it->repeat_count == -1 || it->repeat_count > 1) {
            it->type = kAlarmTypeRepeating;
        } else if (it->hour >= 0) {
            it->type = kAlarmTypeScheduled;
        } else {
            it->type = kAlarmTypeCountdown;
        }
        
        storage_->SaveAlarm(*it);
        
        Reschedule();
        
        alarm = *it;
    }

    NotifyAlarmChange(alarm, "update");
    
    ESP_LOGI(TAG, "Updated alarm id=%u", id);
    return true;
}

Alarm* AlarmManager::GetAlarm(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = std::find_if(alarms_.begin(), alarms_.end(), 
                           [id](const Alarm& a) { return a.id == id; });
    
    if (it == alarms_.end()) {
        return nullptr;
    }
    
    return &(*it);
}

std::vector<Alarm> AlarmManager::GetAllAlarms() {
    std::lock_guard<std::mutex> lock(mutex_);
    return alarms_;
}

bool AlarmManager::ClearAllAlarms() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        storage_->ClearAll();
        alarms_.clear();
        scheduler_->Cancel();
    }

    Alarm empty_alarm;
    NotifyAlarmChange(empty_alarm, "clear");
    
    ESP_LOGI(TAG, "Cleared all alarms");
    return true;
}

std::string AlarmManager::GetAlarmsJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    cJSON* root = cJSON_CreateObject();
    cJSON* alarms_array = cJSON_CreateArray();
    
    for (const auto& alarm : alarms_) {
        cJSON* alarm_json = alarm.ToCjson();
        
        char time_str[32];
        struct tm* tm_info = localtime(&alarm.trigger_time);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
        cJSON_AddStringToObject(alarm_json, "trigger_time_str", time_str);
        
        const char* type_str = "countdown";
        if (alarm.type == kAlarmTypeScheduled) {
            type_str = "scheduled";
        } else if (alarm.type == kAlarmTypeRepeating) {
            type_str = "repeating";
        }
        cJSON_AddStringToObject(alarm_json, "type_str", type_str);
        
        cJSON_AddItemToArray(alarms_array, alarm_json);
    }
    
    cJSON_AddItemToObject(root, "alarms", alarms_array);
    cJSON_AddNumberToObject(root, "total", alarms_.size());
    
    char* json_str = cJSON_PrintUnformatted(root);
    std::string result(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    return result;
}

void AlarmManager::OnAlarmTriggered(const Alarm& alarm) {
    ESP_LOGI(TAG, "Alarm triggered: id=%u, name=%s", alarm.id, alarm.name.c_str());
    
    if (alarm_callback_) {
        alarm_callback_(alarm);
    }
    
    bool should_delete = false;
    Alarm deleted_alarm = alarm;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = std::find_if(alarms_.begin(), alarms_.end(), 
                               [id = alarm.id](const Alarm& a) { return a.id == id; });
        
        if (it != alarms_.end()) {
            if (it->repeat_count == -1) {
                it->trigger_time += it->interval;
                storage_->SaveAlarm(*it);
                ESP_LOGI(TAG, "Repeating alarm rescheduled: id=%u", alarm.id);
            } else if (it->repeat_count > 1) {
                it->repeat_count--;
                it->trigger_time += it->interval;
                storage_->SaveAlarm(*it);
                ESP_LOGI(TAG, "Repeating alarm rescheduled: id=%u, remaining=%d", 
                         alarm.id, it->repeat_count);
            } else {
                deleted_alarm = *it;
                storage_->DeleteAlarm(alarm.id);
                alarms_.erase(it);
                should_delete = true;
                ESP_LOGI(TAG, "One-time alarm removed: id=%u", alarm.id);
            }
        }
        
        Reschedule();
    }
    
    if (should_delete) {
        NotifyAlarmChange(deleted_alarm, "delete");
    }
}

void AlarmManager::Reschedule() {
    scheduler_->ScheduleNext(alarms_);
}

void AlarmManager::SaveAllAlarms() {
    storage_->ClearAll();
    for (const auto& alarm : alarms_) {
        storage_->SaveAlarm(alarm);
    }
}
