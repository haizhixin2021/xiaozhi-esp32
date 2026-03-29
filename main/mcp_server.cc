/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <cstring>
#include <esp_pthread.h>

#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "board.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"
#include "alarm_clock.h"
#include "alarm_cloud_sync.h"
#include "boards/common/esp32_music.h"

#define TAG "MCP"

McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }

    auto music = board.GetMusic();
    if (music) {
        AddTool("self.music.play_song",
             "播放指定的歌曲。当用户要求播放音乐时使用此工具，会自动获取歌曲详情并开始流式播放。\n"
             "参数:\n"
             "  `song_name`: 要播放的歌曲名称（必需）。\n"
             "  `artist_name`: 要播放的歌曲艺术家名称（可选，默认为空字符串）。\n"
             "返回:\n"
             "  播放状态信息，不需确认，立刻播放歌曲。",
             PropertyList({
                 Property("song_name", kPropertyTypeString),//歌曲名称（必需）
                 Property("artist_name", kPropertyTypeString, "")//艺术家名称（可选，默认为空字符串）
             }),
             [music](const PropertyList& properties) -> ReturnValue {
                 auto song_name = properties["song_name"].value<std::string>();
                 auto artist_name = properties["artist_name"].value<std::string>();
                 
                 if (!music->Download(song_name, artist_name)) {
                     return "{\"success\": false, \"message\": \"获取音乐资源失败\"}";
                 }
                 auto download_result = music->GetDownloadResult();
                 ESP_LOGI(TAG, "Music details result: %s", download_result.c_str());
                 return "{\"success\": true, \"message\": \"音乐开始播放\"}";
             });
 
        AddTool("self.music.set_display_mode",
             "设置音乐播放时的显示模式。可以选择显示频谱或歌词，比如用户说‘打开频谱’或者‘显示频谱’，‘打开歌词’或者‘显示歌词’就设置对应的显示模式。\n"
             "参数:\n"
             "  `mode`: 显示模式，可选值为 'spectrum'（频谱）或 'lyrics'（歌词）。\n"
             "返回:\n"
             "  设置结果信息。",
             PropertyList({
                 Property("mode", kPropertyTypeString)//显示模式: "spectrum" 或 "lyrics"
             }),
             [music](const PropertyList& properties) -> ReturnValue {
                 auto mode_str = properties["mode"].value<std::string>();
                 
                 // 转换为小写以便比较
                 std::transform(mode_str.begin(), mode_str.end(), mode_str.begin(), ::tolower);
                 
                 if (mode_str == "spectrum" || mode_str == "频谱") {
                     // 设置为频谱显示模式
                     auto esp32_music = static_cast<Esp32Music*>(music);
                     esp32_music->SetDisplayMode(Esp32Music::DISPLAY_MODE_SPECTRUM);
                     return "{\"success\": true, \"message\": \"已切换到频谱显示模式\"}";
                 } else if (mode_str == "lyrics" || mode_str == "歌词") {
                     // 设置为歌词显示模式
                     auto esp32_music = static_cast<Esp32Music*>(music);
                     esp32_music->SetDisplayMode(Esp32Music::DISPLAY_MODE_LYRICS);
                     return "{\"success\": true, \"message\": \"已切换到歌词显示模式\"}";
                 } else {
                     return "{\"success\": false, \"message\": \"无效的显示模式，请使用 'spectrum' 或 'lyrics'\"}";
                 }
                 
                 return "{\"success\": false, \"message\": \"设置显示模式失败\"}";
             });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    return true;
                }
                return false;
            });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
            "Always remember you have a camera. If the user asks you to see something, use this tool to take a photo and then explain it.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                // Lower the priority to do the camera capture
                TaskPriorityReset priority_reset(1);

                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
    }
#endif

    AddAlarmTools();

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddAlarmTools() {
    AddTool("self.alarm.add",
        "Add a new alarm. Supports three modes:\n"
        "1. Countdown mode: Set delay seconds to trigger after a delay\n"
        "2. Scheduled mode: Set hour and minute to trigger at a specific time\n"
        "3. Repeating mode: Set repeat count and interval for recurring alarms\n"
        "Priority: hour/minute > delay. If both are provided, hour/minute takes precedence.\n"
        "Args:\n"
        "  `name`: The alarm name or reminder content (required)\n"
        "  `delay`: Delay in seconds for countdown mode (optional, default 60)\n"
        "  `hour`: Hour (0-23) for scheduled mode, use 255 if not specified (optional, default 255)\n"
        "  `minute`: Minute (0-59) for scheduled mode, use 255 if not specified (optional, default 255)\n"
        "  `repeat`: Repeat count, 1 for one-time, -1 for infinite, default 1 (optional)\n"
        "  `interval`: Interval in seconds between repeats, default 86400 (24 hours) (optional)\n"
        "Return:\n"
        "  A JSON object with alarm details including id and trigger time.",
        PropertyList({
            Property("name", kPropertyTypeString),
            Property("delay", kPropertyTypeInteger, 60, 1, 86400 * 365),
            Property("hour", kPropertyTypeInteger, 255, 0, 255),
            Property("minute", kPropertyTypeInteger, 255, 0, 255),
            Property("repeat", kPropertyTypeInteger, 1, -1, 10000),
            Property("interval", kPropertyTypeInteger, 86400, 1, 86400 * 365)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto name = properties["name"].value<std::string>();
            int delay = properties["delay"].value<int>();
            int hour = properties["hour"].value<int>();
            int minute = properties["minute"].value<int>();
            int repeat = properties["repeat"].value<int>();
            int interval = properties["interval"].value<int>();

            if (hour == 255) hour = -1;
            if (minute == 255) minute = -1;

            auto& manager = AlarmManager::GetInstance();
            Alarm alarm = manager.AddAlarm(name, delay, hour, minute, repeat, interval);
            
            cJSON* json = alarm.ToCjson();
            char time_str[32];
            struct tm* tm_info = localtime(&alarm.trigger_time);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
            cJSON_AddStringToObject(json, "trigger_time_str", time_str);
            cJSON_AddStringToObject(json, "message", "Alarm added successfully");
            return json;
        });

    AddTool("self.alarm.list",
        "Get all alarms list.\n"
        "Return:\n"
        "  A JSON object with alarms array and total count.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& manager = AlarmManager::GetInstance();
            std::string json_str = manager.GetAlarmsJson();
            cJSON* json = cJSON_Parse(json_str.c_str());
            return json;
        });

    AddTool("self.alarm.delete",
        "Delete an alarm by ID.\n"
        "Args:\n"
        "  `id`: The alarm ID to delete (required)\n"
        "Return:\n"
        "  A JSON object with success status.",
        PropertyList({
            Property("id", kPropertyTypeInteger, 1, 1, 999999)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            uint32_t id = (uint32_t)properties["id"].value<int>();
            auto& manager = AlarmManager::GetInstance();
            bool success = manager.RemoveAlarm(id);
            
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "success", success);
            cJSON_AddStringToObject(json, "message", success ? "Alarm deleted" : "Alarm not found");
            return json;
        });

    AddTool("self.alarm.update",
        "Update an existing alarm.\n"
        "Args:\n"
        "  `id`: The alarm ID to update (required)\n"
        "  `name`: New alarm name (optional)\n"
        "  `hour`: New hour (0-23) (optional)\n"
        "  `minute`: New minute (0-59) (optional)\n"
        "  `repeat`: New repeat count (optional)\n"
        "  `interval`: New interval in seconds (optional)\n"
        "Return:\n"
        "  A JSON object with success status.",
        PropertyList({
            Property("id", kPropertyTypeInteger, 1, 1, 999999),
            Property("name", kPropertyTypeString, ""),
            Property("hour", kPropertyTypeInteger, 255, 0, 255),
            Property("minute", kPropertyTypeInteger, 255, 0, 255),
            Property("repeat", kPropertyTypeInteger, -2, -2, 10000),
            Property("interval", kPropertyTypeInteger, 0, 0, 86400 * 365)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            uint32_t id = (uint32_t)properties["id"].value<int>();
            auto name = properties["name"].value<std::string>();
            int hour = properties["hour"].value<int>();
            int minute = properties["minute"].value<int>();
            int repeat = properties["repeat"].value<int>();
            int interval = properties["interval"].value<int>();

            if (hour == 255) hour = -1;
            if (minute == 255) minute = -1;
            if (repeat == -2) repeat = 0;

            auto& manager = AlarmManager::GetInstance();
            bool success = manager.UpdateAlarm(id, name, hour, minute, repeat, interval);
            
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "success", success);
            cJSON_AddStringToObject(json, "message", success ? "Alarm updated" : "Alarm not found");
            return json;
        });

    AddTool("self.alarm.clear",
        "Clear all alarms.\n"
        "Return:\n"
        "  A JSON object with success status.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& manager = AlarmManager::GetInstance();
            bool success = manager.ClearAllAlarms();
            
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "success", success);
            cJSON_AddStringToObject(json, "message", "All alarms cleared");
            return json;
        });

    AddTool("self.alarm.stop_ring",
        "Stop the alarm ring sound.\n"
        "Return:\n"
        "  A JSON object with success status.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.StopAlarmRing();
            
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "success", true);
            cJSON_AddStringToObject(json, "message", "Alarm ring stopped");
            return json;
        });
        // 新增：云端同步工具
    AddTool("self.alarm.sync_cloud",
        "Sync alarms with cloud server.\n"
        "Args:\n"
        "  `direction`: Sync direction - 'to_cloud', 'from_cloud', or 'full' (default: 'full')\n"
        "Return:\n"
        "  A JSON object with sync result.",
        PropertyList({
            Property("direction", kPropertyTypeString, "full")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto direction = properties["direction"].value<std::string>();
            auto& sync = AlarmCloudSync::GetInstance();
            
            cJSON* json = cJSON_CreateObject();
            
            if (direction == "to_cloud") {
                sync.SyncToCloud([](bool success, const std::string& msg) {
                    ESP_LOGI(TAG, "Sync to cloud: %s", msg.c_str());
                });
                cJSON_AddStringToObject(json, "message", "Sync to cloud initiated");
            } else if (direction == "from_cloud") {
                sync.SyncFromCloud([](bool success, const std::string& msg) {
                    ESP_LOGI(TAG, "Sync from cloud: %s", msg.c_str());
                });
                cJSON_AddStringToObject(json, "message", "Sync from cloud initiated");
            } else {
                sync.FullSync([](bool success, const std::string& msg) {
                    ESP_LOGI(TAG, "Full sync: %s", msg.c_str());
                });
                cJSON_AddStringToObject(json, "message", "Full sync initiated");
            }
            
            cJSON_AddBoolToObject(json, "success", true);
            return json;
        });
        
        // 新增：在线音乐播放工具
    /* AddTool("self.online_music.play",
        "播放在线音乐。当用户要求播放歌曲、音乐时使用此工具。\n"
        "此工具会从在线音乐服务搜索并播放用户指定的歌曲。\n"
        "Args:\n"
        "  `song_name`: 要播放的歌曲名称（必需）。\n"
        "  `singer`: 歌手名称（可选，默认为空）。\n"
        "返回:\n"
        "  播放状态信息，包含成功/失败状态和消息。\n"
        "示例:\n"
        "  - 播放《青花瓷》：song_name=\"青花瓷\", singer=\"周杰伦\"\n"
        "  - 播放《稻香》：song_name=\"稻香\", singer=\"周杰伦\"",
        PropertyList({
            Property("song_name", kPropertyTypeString),
            Property("singer", kPropertyTypeString, "")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            auto song_name = properties["song_name"].value<std::string>();
            auto singer = properties["singer"].value<std::string>();
            
            if (!app.PlayMusic(song_name, singer)) {
                return "{\"success\": false, \"message\": \"获取音乐资源失败\"}";
            }
            return "{\"success\": true, \"message\": \"音乐开始播放\"}";
        });
        
    AddTool("self.online_music.stop",
        "停止当前播放的在线音乐。当用户要求停止音乐时使用此工具。\n"
        "返回:\n"
        "  停止状态信息，包含成功/失败状态和消息。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.StopMusic();
            return "{\"success\": true, \"message\": \"音乐已停止\"}";
        });
        
    AddTool("self.online_music.status",
        "获取当前在线音乐播放状态。当用户询问音乐状态时使用此工具。\n"
        "返回:\n"
        "  当前播放状态信息，包含playing（是否正在播放）、paused（是否暂停）和message（状态描述）。\n"
        "状态说明:\n"
        "  - playing=true, paused=false: 正在播放音乐\n"
        "  - playing=false, paused=true: 音乐已暂停\n"
        "  - playing=false, paused=false: 未在播放音乐",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            bool is_playing = app.IsMusicPlaying();
            bool is_paused = app.IsMusicPaused();
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "playing", is_playing);
            cJSON_AddBoolToObject(json, "paused", is_paused);
            cJSON_AddStringToObject(json, "message", is_playing ? "正在播放音乐" : (is_paused ? "音乐已暂停" : "未在播放音乐"));
            return json;
        });
        
    AddTool("self.online_music.pause",
        "暂停当前播放的在线音乐。当用户要求暂停音乐时使用此工具。\n"
        "返回:\n"
        "  暂停状态信息，包含成功/失败状态和消息。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.PauseMusic();
            return "{\"success\": true, \"message\": \"音乐已暂停\"}";
        });
        
    AddTool("self.online_music.resume",
        "继续播放暂停的在线音乐。当用户要求继续播放、恢复播放时使用此工具。\n"
        "返回:\n"
        "  继续播放状态信息，包含成功/失败状态和消息。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.ResumeMusic();
            return "{\"success\": true, \"message\": \"音乐继续播放\"}";
        }); */
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info",
        "Get the system information",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddTool("self.stop_conversation",
        "Stop the current conversation and return to standby mode. Use this when the user says 'stop', 'close', 'goodbye', or wants to end the conversation.",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            
            ESP_LOGI(TAG, "Stop conversation requested, current state: %d", (int)state);
            
            if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
                app.Schedule([&app]() {
                    ESP_LOGI(TAG, "Stopping conversation and returning to standby");
                    app.ToggleChatState();
                });
                return "{\"success\": true, \"message\": \"Conversation stopped, returning to standby\"}";
            } else if (state == kDeviceStateIdle) {
                return "{\"success\": true, \"message\": \"Already in standby mode\"}";
            } else {
                return "{\"success\": false, \"message\": \"Cannot stop conversation in current state\"}";
            }
        });

    AddUserOnlyTool("self.reboot", "Reboot the system",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot();
            });
            return true;
        });

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());
            
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                bool success = app.UpgradeFirmware(url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });
            
            return true;
        });

    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
            PropertyList(),
            [display](const PropertyList& properties) -> ReturnValue {
                cJSON *json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "width", display->width());
                cJSON_AddNumberToObject(json, "height", display->height());
                if (dynamic_cast<OledDisplay*>(display)) {
                    cJSON_AddBoolToObject(json, "monochrome", true);
                } else {
                    cJSON_AddBoolToObject(json, "monochrome", false);
                }
                return json;
            });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool("self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("quality", kPropertyTypeInteger, 80, 1, 100)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    throw std::runtime_error("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());
                
                // 构造multipart/form-data请求体
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (!http->Open("POST", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    http->Write(file_header.c_str(), file_header.size());
                }

                // JPEG数据
                http->Write((const char*)jpeg_data.data(), jpeg_data.size());

                {
                    // multipart尾部
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    http->Write(multipart_footer.c_str(), multipart_footer.size());
                }
                http->Write("", 0);

                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                return true;
            });
        
        AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                }

                size_t content_length = http->GetBodyLength();
                char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                if (data == nullptr) {
                    throw std::runtime_error("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    int ret = http->Read(data + total_read, content_length - total_read);
                    if (ret < 0) {
                        heap_caps_free(data);
                        throw std::runtime_error("Failed to download image: " + url);
                    }
                    if (ret == 0) {
                        break;
                    }
                    total_read += ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif // CONFIG_LV_USE_SNAPSHOT
    }
#endif // HAVE_LVGL

    // Assets download url
    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
        AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });
    }
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }
    
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
}
