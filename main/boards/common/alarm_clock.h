#ifndef ALARM_CLOCK_H
#define ALARM_CLOCK_H

#include <string>
#include <vector>
#include <functional>
#include <ctime>
#include <cstdint>
#include <mutex>
#include <memory>

#include <esp_timer.h>
#include <cJSON.h>

enum AlarmType {
    kAlarmTypeCountdown = 0,
    kAlarmTypeScheduled = 1,
    kAlarmTypeRepeating = 2
};

struct Alarm {
    uint32_t id;
    time_t trigger_time;
    int16_t hour;
    int16_t minute;
    int32_t repeat_count;
    int32_t interval;
    std::string name;
    bool enabled;
    uint8_t type;

    Alarm() : id(0), trigger_time(0), hour(-1), minute(-1), 
              repeat_count(1), interval(0), enabled(true), type(kAlarmTypeCountdown) {}

    std::string ToJson() const;
    static Alarm FromJson(const std::string& json);
    cJSON* ToCjson() const;
    static Alarm FromCjson(const cJSON* json);
};

class AlarmStorage {
public:
    AlarmStorage();
    ~AlarmStorage();

    bool LoadAll(std::vector<Alarm>& alarms);
    bool SaveAlarm(const Alarm& alarm);
    bool DeleteAlarm(uint32_t id);
    bool ClearAll();
    uint32_t GetNextId();

private:
    static constexpr const char* kNamespace = "alarm_clock";
    static constexpr const char* kCountKey = "count";
    static constexpr const char* kNextIdKey = "next_id";
    static constexpr int kMaxAlarms = 50;
};

class AlarmManager;

class AlarmScheduler {
public:
    using AlarmCallback = std::function<void(const Alarm&)>;

    AlarmScheduler();
    ~AlarmScheduler();

    void SetCallback(AlarmCallback callback);
    void ScheduleNext(const std::vector<Alarm>& alarms);
    void Cancel();
    bool IsScheduled() const { return timer_handle_ != nullptr; }

private:
    static void TimerCallback(void* arg);
    void OnTimerTriggered();

    esp_timer_handle_t timer_handle_ = nullptr;
    AlarmCallback callback_;
    Alarm pending_alarm_;
};

class AlarmManager {
public:
    using AlarmCallback = std::function<void(const Alarm&)>;
    using AlarmChangeCallback = std::function<void(const Alarm&, const std::string&)>;

    static AlarmManager& GetInstance();

    void Initialize();
    void SetAlarmCallback(AlarmCallback callback);
    void SetAlarmChangeCallback(AlarmChangeCallback callback);

    Alarm AddAlarm(const std::string& name, int delay, int hour, int minute, 
                   int repeat, int interval);
    bool RemoveAlarm(uint32_t id);
    bool UpdateAlarm(uint32_t id, const std::string& name, int hour, int minute,
                     int repeat, int interval);
    Alarm* GetAlarm(uint32_t id);
    std::vector<Alarm> GetAllAlarms();
    bool ClearAllAlarms();
    std::string GetAlarmsJson();
    int GetAlarmCount() const { return alarms_.size(); }

    void OnAlarmTriggered(const Alarm& alarm);

private:
    AlarmManager();
    ~AlarmManager();

    void Reschedule();
    void SaveAllAlarms();
    void NotifyAlarmChange(const Alarm& alarm, const std::string& action);

    std::mutex mutex_;
    std::vector<Alarm> alarms_;
    std::unique_ptr<AlarmStorage> storage_;
    std::unique_ptr<AlarmScheduler> scheduler_;
    AlarmCallback alarm_callback_;
    AlarmChangeCallback alarm_change_callback_;
};

#endif
