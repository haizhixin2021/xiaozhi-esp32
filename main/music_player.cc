#include "music_player.h"
#include "board.h"
#include "display.h"
#include <esp_log.h>
#include <cJSON.h>
#include <memory>

#define TAG "MusicPlayer"

// 事件定义
#define EVENT_DOWNLOAD_STARTED   (1 << 0)
#define EVENT_DOWNLOAD_FINISHED  (1 << 1)
#define EVENT_DECODE_STARTED     (1 << 2)
#define EVENT_DECODE_FINISHED    (1 << 3)
#define EVENT_LYRICS_LOADED      (1 << 4)

MusicPlayer::MusicPlayer(AudioService& audio_service) 
    : audio_service_(audio_service),
      is_playing_(false),
      is_stopped_(false),
      is_paused_(false),
      download_task_handle_(nullptr),
      decode_task_handle_(nullptr),
      lyrics_sync_task_handle_(nullptr),
      current_time_ms_(0) {
    
    event_group_ = xEventGroupCreate();
}

MusicPlayer::~MusicPlayer() {
    StopStreaming();
    if (event_group_) {
        vEventGroupDelete(event_group_);
    }
}

bool MusicPlayer::Download(const std::string& song_name, const std::string& singer) {
    if (is_playing_) {
        ESP_LOGW(TAG, "Music is already playing");
        return false;
    }
    
    current_song_name_ = song_name;
    current_singer_ = singer;
    is_playing_ = true;
    is_stopped_ = false;
    
    // 启动下载任务
    xTaskCreate([](void* arg) {
        MusicPlayer* player = static_cast<MusicPlayer*>(arg);
        player->DownloadTask();
        vTaskDelete(NULL);
    }, "music_download", 4096 * 4, this, 5, &download_task_handle_);
    
    return true;
}

void MusicPlayer::StopStreaming() {
    if (!is_playing_) {
        return;
    }
    
    is_stopped_ = true;
    is_playing_ = false;
    is_paused_ = false;
    
    // 等待任务结束
    if (download_task_handle_) {
        xTaskNotifyGive(download_task_handle_);
        vTaskDelete(download_task_handle_);
        download_task_handle_ = nullptr;
    }
    
    if (decode_task_handle_) {
        xTaskNotifyGive(decode_task_handle_);
        vTaskDelete(decode_task_handle_);
        decode_task_handle_ = nullptr;
    }
    
    if (lyrics_sync_task_handle_) {
        xTaskNotifyGive(lyrics_sync_task_handle_);
        vTaskDelete(lyrics_sync_task_handle_);
        lyrics_sync_task_handle_ = nullptr;
    }
    
    // 清空缓冲区
    {   
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        audio_buffer_.clear();
    }
    
    // 清空歌词
    {   
        std::lock_guard<std::mutex> lock(lyric_mutex_);
        lyrics_.clear();
        current_lyric_ = "";
    }
    
    // 清空歌曲信息
    current_song_name_.clear();
    current_singer_.clear();
    current_song_id_.clear();
    current_song_url_.clear();
    current_lyric_url_.clear();
    
    // 重置音频服务
    audio_service_.ResetDecoder();
    
    // 清空显示
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->SetChatMessage("music", "");
    }
}

std::string MusicPlayer::GetCurrentLyric() const {
    std::lock_guard<std::mutex> lock(lyric_mutex_);
    return current_lyric_;
}

void MusicPlayer::DownloadTask() {
    ESP_LOGI(TAG, "Download task started: %s - %s", current_song_name_.c_str(), current_singer_.c_str());
    
    // 1. 搜索音乐
    std::string song_id;
    if (!SearchMusic(current_song_name_, current_singer_, song_id)) {
        ESP_LOGE(TAG, "Search music failed");
        is_playing_ = false;
        return;
    }
    
    current_song_id_ = song_id;
    
    // 2. 获取歌曲播放链接
    std::string song_url;
    if (!GetSongUrl(song_id, song_url)) {
        ESP_LOGE(TAG, "Get song URL failed");
        is_playing_ = false;
        return;
    }
    
    current_song_url_ = song_url;
    
    // 3. 获取歌词
    std::vector<LyricLine> lyrics;
    if (!GetLyric(song_id, lyrics)) {
        ESP_LOGW(TAG, "Get lyric failed, continue without lyrics");
    } else {
        std::lock_guard<std::mutex> lock(lyric_mutex_);
        lyrics_ = lyrics;
        xEventGroupSetBits(event_group_, EVENT_LYRICS_LOADED);
    }
    
    // 4. 启动解码任务
    xTaskCreate([](void* arg) {
        MusicPlayer* player = static_cast<MusicPlayer*>(arg);
        player->DecodeTask();
        vTaskDelete(NULL);
    }, "music_decode", 4096 * 4, this, 4, &decode_task_handle_);
    
    // 5. 启动歌词同步任务
    xTaskCreate([](void* arg) {
        MusicPlayer* player = static_cast<MusicPlayer*>(arg);
        player->LyricsSyncTask();
        vTaskDelete(NULL);
    }, "lyrics_sync", 4096, this, 3, &lyrics_sync_task_handle_);
    
    // 6. 下载音频数据
    auto network = Board::GetInstance().GetNetwork();
    std::string current_url = song_url;
    const int max_redirects = 5;
    int redirect_count = 0;
    std::unique_ptr<Http> http;
    
    while (redirect_count < max_redirects) {
        http = network->CreateHttp(0);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create HTTP client");
            is_playing_ = false;
            return;
        }
        
        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http->SetHeader("Accept", "*/*");
        
        ESP_LOGI(TAG, "Opening URL: %s", current_url.c_str());
        
        if (!http->Open("GET", current_url)) {
            ESP_LOGE(TAG, "Failed to open HTTP connection");
            is_playing_ = false;
            return;
        }
        
        // 获取HTTP状态码
        int status_code = http->GetStatusCode();
        ESP_LOGI(TAG, "HTTP status: %d", status_code);
        
        // 处理重定向
        if (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308) {
            std::string location = http->GetResponseHeader("Location");
            http->Close();
            http.reset();
            
            if (location.empty()) {
                ESP_LOGE(TAG, "Redirect but no Location header found");
                is_playing_ = false;
                return;
            }
            
            ESP_LOGI(TAG, "Redirect %d -> %s", redirect_count + 1, location.c_str());
            current_url = location;
            redirect_count++;
            continue;
        }
        
        if (status_code != 200 && status_code != 206) {
            ESP_LOGE(TAG, "HTTP error: %d", status_code);
            http->Close();
            is_playing_ = false;
            return;
        }
        
        // 成功获取响应，跳出重定向循环
        break;
    }
    
    if (redirect_count >= max_redirects) {
        ESP_LOGE(TAG, "Too many redirects");
        is_playing_ = false;
        return;
    }
    
    ESP_LOGI(TAG, "Final URL: %s", current_url.c_str());
    
    // 流式下载
    const int buffer_size = 4096;
    uint8_t buffer[buffer_size];
    int total_downloaded = 0;
    bool first_chunk = true;
    
    while (!is_stopped_) {
        int read_len = http->Read((char*)buffer, buffer_size);
        if (read_len <= 0) {
            break;
        }
        
        // 打印第一个数据块的前几个字节，用于调试
        if (first_chunk) {
            ESP_LOGI(TAG, "First %d bytes of audio data:", std::min(read_len, 32));
            ESP_LOG_BUFFER_HEX(TAG, buffer, std::min(read_len, 32));
            first_chunk = false;
        }
        
        total_downloaded += read_len;
        
        // 将数据添加到缓冲区
        {   
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            audio_buffer_.insert(audio_buffer_.end(), buffer, buffer + read_len);
            buffer_cv_.notify_one();
        }
        
        // 检查是否需要退出
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)) != 0) {
            break;
        }
    }
    
    http->Close();
    
    ESP_LOGI(TAG, "Download task finished, total downloaded: %d bytes", total_downloaded);
    xEventGroupSetBits(event_group_, EVENT_DOWNLOAD_FINISHED);
}

void MusicPlayer::DecodeTask() {
    ESP_LOGI(TAG, "Decode task started");
    
    // 注册默认解码器
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    
    // 初始化MP3解码器
    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    
    esp_audio_err_t ret = esp_audio_simple_dec_open(&dec_cfg, &mp3_decoder_);
    if (ret != ESP_AUDIO_ERR_OK || mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder: %d", ret);
        return;
    }
    ESP_LOGI(TAG, "MP3 decoder opened successfully");
    
    // 解码缓冲区 - 增大输入缓冲区以容纳完整的MP3帧
    const int in_buffer_size = 8192;
    const int out_buffer_size = 1152 * 4; // MP3 最大帧大小 * 2 声道 * 2 字节
    uint8_t* in_buffer = new uint8_t[in_buffer_size];
    uint8_t* out_buffer = new uint8_t[out_buffer_size];
    
    size_t buffer_pos = 0;
    bool download_finished = false;
    bool first_decode = true;
    bool id3_skipped = false;
    
    while (!is_stopped_) {
        // 检查是否暂停
        while (is_paused_ && !is_stopped_) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        // 从下载缓冲区获取数据
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            if (audio_buffer_.empty()) {
                if (xEventGroupGetBits(event_group_) & EVENT_DOWNLOAD_FINISHED) {
                    download_finished = true;
                } else if (!download_finished) {
                    buffer_cv_.wait_for(lock, std::chrono::milliseconds(100));
                    continue;
                }
            } else {
                // 填充输入缓冲区
                size_t space = in_buffer_size - buffer_pos;
                size_t take_size = std::min(audio_buffer_.size(), space);
                if (take_size > 0) {
                    memcpy(in_buffer + buffer_pos, audio_buffer_.data(), take_size);
                    buffer_pos += take_size;
                    audio_buffer_.erase(audio_buffer_.begin(), audio_buffer_.begin() + take_size);
                }
            }
        }
        
        // 如果没有数据且下载未完成，继续等待
        if (buffer_pos == 0 && !download_finished) {
            continue;
        }
        
        // 如果没有数据且下载已完成，退出
        if (buffer_pos == 0 && download_finished) {
            ESP_LOGI(TAG, "All data processed, exiting decode task");
            break;
        }
        
        // 跳过 ID3v2 标签
        if (!id3_skipped && buffer_pos >= 10) {
            if (in_buffer[0] == 'I' && in_buffer[1] == 'D' && in_buffer[2] == '3') {
                // ID3v2 标签格式: "ID3" + version(2) + flags(1) + size(4, syncsafe)
                uint32_t id3_size = ((in_buffer[6] & 0x7F) << 21) |
                                    ((in_buffer[7] & 0x7F) << 14) |
                                    ((in_buffer[8] & 0x7F) << 7) |
                                    (in_buffer[9] & 0x7F);
                id3_size += 10; // 加上头部10字节
                
                ESP_LOGI(TAG, "Found ID3v2 tag, size: %u bytes", id3_size);
                
                // 等待足够的数据
                while (buffer_pos < id3_size && !download_finished) {
                    std::unique_lock<std::mutex> lock(buffer_mutex_);
                    if (!audio_buffer_.empty()) {
                        size_t space = in_buffer_size - buffer_pos;
                        size_t take_size = std::min(audio_buffer_.size(), space);
                        if (take_size > 0) {
                            memcpy(in_buffer + buffer_pos, audio_buffer_.data(), take_size);
                            buffer_pos += take_size;
                            audio_buffer_.erase(audio_buffer_.begin(), audio_buffer_.begin() + take_size);
                        }
                    } else {
                        buffer_cv_.wait_for(lock, std::chrono::milliseconds(100));
                    }
                }
                
                // 跳过 ID3 标签
                if (buffer_pos >= id3_size) {
                    size_t remain = buffer_pos - id3_size;
                    if (remain > 0) {
                        memmove(in_buffer, in_buffer + id3_size, remain);
                    }
                    buffer_pos = remain;
                    id3_skipped = true;
                    ESP_LOGI(TAG, "ID3v2 tag skipped, remaining data: %u bytes", (unsigned int)buffer_pos);
                }
            } else {
                id3_skipped = true;
            }
        }
        
        // 查找 MP3 帧同步字 (0xFF 0xFB 或 0xFF 0xFA 或 0xFF 0xFx)
        if (id3_skipped && buffer_pos >= 2) {
            size_t sync_pos = 0;
            while (sync_pos < buffer_pos - 1) {
                if (in_buffer[sync_pos] == 0xFF && (in_buffer[sync_pos + 1] & 0xE0) == 0xE0) {
                    break;
                }
                sync_pos++;
            }
            
            if (sync_pos > 0) {
                ESP_LOGI(TAG, "Skipping %u bytes to find MP3 sync", (unsigned int)sync_pos);
                size_t remain = buffer_pos - sync_pos;
                if (remain > 0) {
                    memmove(in_buffer, in_buffer + sync_pos, remain);
                }
                buffer_pos = remain;
            }
        }
        
        // 如果数据不足，继续等待（MP3帧通常需要至少几百字节）
        if (buffer_pos < 1024 && !download_finished) {
            continue;
        }
        
        // 打印第一个数据块的前几个字节，用于调试
        if (first_decode && buffer_pos > 0) {
            ESP_LOGI(TAG, "First %u bytes to decode:", (unsigned int)std::min(buffer_pos, (size_t)32));
            ESP_LOG_BUFFER_HEX(TAG, in_buffer, std::min(buffer_pos, (size_t)32));
            first_decode = false;
        }
        
        // 解码MP3数据
        esp_audio_simple_dec_raw_t raw = {
            .buffer = in_buffer,
            .len = (uint32_t)buffer_pos,
            .eos = download_finished,
            .consumed = 0,
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
        };
        
        esp_audio_simple_dec_out_t out = {
            .buffer = out_buffer,
            .len = out_buffer_size,
            .needed_size = 0,
            .decoded_size = 0,
        };
        
        ret = esp_audio_simple_dec_process(mp3_decoder_, &raw, &out);
        
        ESP_LOGI(TAG, "Decode result: %d, consumed: %u, decoded: %u, buffer_pos: %u", 
                 ret, raw.consumed, out.decoded_size, (unsigned int)buffer_pos);
        
        // 处理已消耗的数据
        if (raw.consumed > 0 && raw.consumed <= buffer_pos) {
            size_t remain = buffer_pos - raw.consumed;
            if (remain > 0) {
                memmove(in_buffer, in_buffer + raw.consumed, remain);
            }
            buffer_pos = remain;
        }
        
        // 获取解码信息
        esp_audio_simple_dec_info_t dec_info = {};
        esp_audio_simple_dec_get_info(mp3_decoder_, &dec_info);
        
        // 只有在解码成功且有数据时才播放
        if (ret == ESP_AUDIO_ERR_OK && out.decoded_size > 0 && dec_info.sample_rate > 0 && dec_info.channel > 0) {
            // 更新解码信息
            if (dec_info.sample_rate != (uint32_t)decoder_sample_rate_) {
                decoder_sample_rate_ = dec_info.sample_rate;
                ESP_LOGI(TAG, "Sample rate: %lu", dec_info.sample_rate);
            }
            if (dec_info.channel > 0 && dec_info.channel != decoder_channels_) {
                decoder_channels_ = dec_info.channel;
                ESP_LOGI(TAG, "Channels: %d", dec_info.channel);
            }
            
            int pcm_samples = out.decoded_size / sizeof(int16_t);
            ESP_LOGI(TAG, "Playing PCM: %d samples, rate: %d, channels: %d", pcm_samples, decoder_sample_rate_, decoder_channels_);
            
            // 将解码后的PCM数据复制到vector
            std::vector<int16_t> pcm_data((int16_t*)out_buffer, (int16_t*)out_buffer + pcm_samples);
            
            // 直接播放PCM数据（包含声道信息）
            audio_service_.PlayPcmData(std::move(pcm_data), decoder_sample_rate_, decoder_channels_);
            
            // 更新当前播放时间（单声道样本数）
            int mono_samples = pcm_samples / decoder_channels_;
            int frame_duration_ms = (mono_samples * 1000) / decoder_sample_rate_;
            current_time_ms_ += frame_duration_ms;
        } else if (ret == ESP_AUDIO_ERR_DATA_LACK) {
            // 数据不足，需要等待更多数据，这是正常情况
            ESP_LOGD(TAG, "Data not enough, waiting for more data");
        } else if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "Decode error: %d, consumed: %u, buffer_pos: %u", ret, raw.consumed, (unsigned int)buffer_pos);
        } else if (out.decoded_size > 0) {
            ESP_LOGI(TAG, "Decoded %u bytes, sample_rate: %lu, channels: %d", 
                     out.decoded_size, dec_info.sample_rate, dec_info.channel);
        }
        
        if (download_finished && buffer_pos == 0) {
            break;
        }
        
        // 检查是否需要退出
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)) != 0) {
            break;
        }
    }
    
    // 清理解码器
    delete[] in_buffer;
    delete[] out_buffer;
    if (mp3_decoder_) {
        esp_audio_simple_dec_close(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    
    ESP_LOGI(TAG, "Decode task finished");
    xEventGroupSetBits(event_group_, EVENT_DECODE_FINISHED);
}

void MusicPlayer::LyricsSyncTask() {
    ESP_LOGI(TAG, "Lyrics sync task started");
    
    // 等待歌词加载完成
    xEventGroupWaitBits(event_group_, EVENT_LYRICS_LOADED, pdFALSE, pdTRUE, portMAX_DELAY);
    
    auto display = Board::GetInstance().GetDisplay();
    
    while (!is_stopped_) {
        // 检查是否暂停
        if (!is_paused_) {
            std::string lyric;
            {
                std::lock_guard<std::mutex> lock(lyric_mutex_);
                lyric = GetLyricAtTime(lyrics_, current_time_ms_);
                if (lyric != current_lyric_) {
                    current_lyric_ = lyric;
                    
                    // 更新显示
                    if (display) {
                        display->SetChatMessage("music", current_lyric_.c_str());
                    }
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(100)); // 每100ms检查一次
        
        // 检查是否需要退出
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)) != 0) {
            break;
        }
    }
    
    ESP_LOGI(TAG, "Lyrics sync task finished");
}

bool MusicPlayer::SearchMusic(const std::string& song_name, const std::string& singer, std::string& song_id) {
    std::string keyword = song_name;
    if (!singer.empty()) {
        keyword += " " + singer;
    }
    
    // 构建符合MCP tools/call格式的参数
    std::string arguments = "{";
    arguments += "\"keyword\": \"" + keyword + "\",";
    arguments += "\"source\": \"netease\",";
    arguments += "\"count\": 10";
    arguments += "}";
    
    std::string response = SendMcpToolRequest("search_music", arguments);
    
    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse search response");
        return false;
    }
    
    // MCP tools/call 响应在 result 中
    cJSON* result = cJSON_GetObjectItem(root, "result");
    if (!result) {
        ESP_LOGE(TAG, "No result in search response");
        cJSON_Delete(root);
        return false;
    }
    
    // 检查是否有错误
    cJSON* error = cJSON_GetObjectItem(root, "error");
    if (error) {
        cJSON* error_msg = cJSON_GetObjectItem(error, "message");
        if (error_msg && cJSON_IsString(error_msg)) {
            ESP_LOGE(TAG, "MCP error: %s", error_msg->valuestring);
        }
        cJSON_Delete(root);
        return false;
    }
    
    // 解析 result 中的 content
    cJSON* content = cJSON_GetObjectItem(result, "content");
    if (!content) {
        ESP_LOGE(TAG, "No content in result");
        cJSON_Delete(root);
        return false;
    }
    
    // content 是数组，取第一个 text 项
    if (cJSON_IsArray(content) && cJSON_GetArraySize(content) > 0) {
        cJSON* first_content = cJSON_GetArrayItem(content, 0);
        cJSON* text = cJSON_GetObjectItem(first_content, "text");
        if (text && cJSON_IsString(text)) {
            // 解析 text 中的 JSON 字符串（这是一个歌曲对象，不是数组）
            cJSON* song = cJSON_Parse(text->valuestring);
            if (song && cJSON_IsObject(song)) {
                cJSON* title = cJSON_GetObjectItem(song, "title");
                cJSON* url = cJSON_GetObjectItem(song, "url");
                cJSON* lrc = cJSON_GetObjectItem(song, "lrc");
                
                if (title && cJSON_IsString(title)) {
                    ESP_LOGI(TAG, "Found song: %s", title->valuestring);
                }
                
                if (url && cJSON_IsString(url)) {
                    song_id = url->valuestring;
                    ESP_LOGI(TAG, "Song URL: %s", url->valuestring);
                }
                
                // 保存歌词URL供后续使用
                if (lrc && cJSON_IsString(lrc)) {
                    current_lyric_url_ = lrc->valuestring;
                    ESP_LOGI(TAG, "Lyric URL: %s", lrc->valuestring);
                }
                
                if (!song_id.empty()) {
                    cJSON_Delete(song);
                    cJSON_Delete(root);
                    return true;
                }
                cJSON_Delete(song);
            }
        }
    }
    
    ESP_LOGE(TAG, "Failed to parse song data");
    cJSON_Delete(root);
    return false;
}

bool MusicPlayer::GetSongUrl(const std::string& song_id, std::string& song_url) {
    // 直接使用song_id作为URL，因为我们已经在SearchMusic中获取了完整的URL
    song_url = song_id;
    ESP_LOGI(TAG, "Using song URL: %s", song_url.c_str());
    return true;
}

bool MusicPlayer::GetLyric(const std::string& song_id, std::vector<LyricLine>& lyrics) {
    // 使用SearchMusic中保存的歌词URL直接下载
    if (current_lyric_url_.empty()) {
        ESP_LOGW(TAG, "No lyric URL available");
        return false;
    }
    
    ESP_LOGI(TAG, "Downloading lyric from: %s", current_lyric_url_.c_str());
    
    // 使用项目封装的HTTP客户端
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "*/*");
    
    if (!http->Open("GET", current_lyric_url_)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection for lyric");
        return false;
    }
    
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to download lyric, status: %d", status_code);
        http->Close();
        return false;
    }
    
    std::string lyric_content = http->ReadAll();
    http->Close();
    
    if (!lyric_content.empty()) {
        ParseLyrics(lyric_content, lyrics);
        ESP_LOGI(TAG, "Got %d lyric lines", lyrics.size());
        return true;
    }
    
    ESP_LOGE(TAG, "Failed to download lyric");
    return false;
}

std::string MusicPlayer::SendMcpToolRequest(const std::string& tool_name, const std::string& arguments) {
    // 构建 MCP tools/call JSON-RPC 请求
    std::string request = "{";
    request += "\"jsonrpc\": \"2.0\",";
    request += "\"method\": \"tools/call\",";
    request += "\"params\": {";
    request += "\"name\": \"" + tool_name + "\",";
    request += "\"arguments\": " + arguments;
    request += "},";
    request += "\"id\": 1";
    request += "}";
    
    ESP_LOGI(TAG, "========== MCP Request Start ==========");
    ESP_LOGI(TAG, "URL: %s", MCP_SERVICE_URL);
    ESP_LOGI(TAG, "Method: POST");
    ESP_LOGI(TAG, "Headers:");
    ESP_LOGI(TAG, "  Content-Type: application/json");
    ESP_LOGI(TAG, "  Accept: application/json");
    ESP_LOGI(TAG, "  Authorization: Bearer %s", MCP_AUTH_TOKEN);
    ESP_LOGI(TAG, "Body: %s", request.c_str());
    ESP_LOGI(TAG, "Body length: %d", (int)request.size());
    ESP_LOGI(TAG, "========== MCP Request End ==========");
    
    // 使用项目封装的HTTP客户端
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Accept", "application/json");
    
    // 添加认证头
    std::string auth_header = "Bearer " + std::string(MCP_AUTH_TOKEN);
    http->SetHeader("Authorization", auth_header.c_str());
    
    // 设置请求体
    http->SetContent(std::move(request));
    
    // 打开连接并发送请求
    if (!http->Open("POST", MCP_SERVICE_URL)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return "";
    }
    
    // 获取响应状态码
    int status_code = http->GetStatusCode();
    size_t content_length = http->GetBodyLength();
    
    ESP_LOGI(TAG, "========== MCP Response Start ==========");
    ESP_LOGI(TAG, "HTTP status code: %d", status_code);
    ESP_LOGI(TAG, "Content-Length: %zu", content_length);
    
    // 读取响应
    std::string response = http->ReadAll();
    
    ESP_LOGI(TAG, "Total read: %zu bytes", response.size());
    ESP_LOGI(TAG, "Response body: %s", response.c_str());
    ESP_LOGI(TAG, "========== MCP Response End ==========");
    
    http->Close();
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status code: %d", status_code);
        return "";
    }
    
    return response;
}

void MusicPlayer::ParseLyrics(const std::string& lrc_content, std::vector<LyricLine>& lyrics) {
    // 简单的LRC歌词解析
    size_t pos = 0;
    while (pos < lrc_content.size()) {
        // 寻找时间标签
        size_t time_start = lrc_content.find('[', pos);
        if (time_start == std::string::npos) {
            break;
        }
        
        size_t time_end = lrc_content.find(']', time_start);
        if (time_end == std::string::npos) {
            break;
        }
        
        // 解析时间
        std::string time_str = lrc_content.substr(time_start + 1, time_end - time_start - 1);
        size_t colon_pos = time_str.find(':');
        if (colon_pos == std::string::npos) {
            pos = time_end + 1;
            continue;
        }
        
        int minute = std::stoi(time_str.substr(0, colon_pos));
        int second = std::stoi(time_str.substr(colon_pos + 1));
        int time_ms = minute * 60 * 1000 + second * 1000;
        
        // 解析歌词文本
        size_t text_start = time_end + 1;
        size_t text_end = lrc_content.find('[', text_start);
        if (text_end == std::string::npos) {
            text_end = lrc_content.size();
        }
        
        std::string text = lrc_content.substr(text_start, text_end - text_start);
        // 去除换行符
        text.erase(std::remove(text.begin(), text.end(), '\n'), text.end());
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        
        if (!text.empty()) {
            LyricLine line;
            line.time_ms = time_ms;
            line.text = text;
            lyrics.push_back(line);
        }
        
        pos = text_end;
    }
}

std::string MusicPlayer::GetLyricAtTime(const std::vector<LyricLine>& lyrics, int current_time_ms) {
    if (lyrics.empty()) {
        return "";
    }
    
    for (size_t i = 0; i < lyrics.size(); i++) {
        if (i == lyrics.size() - 1 || current_time_ms < lyrics[i + 1].time_ms) {
            return lyrics[i].text;
        }
    }
    
    return "";
}

void MusicPlayer::Pause() {
    if (is_playing_ && !is_paused_) {
        ESP_LOGI(TAG, "Pausing music playback");
        is_paused_ = true;
        
        // 显示暂停状态
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->SetChatMessage("music", "音乐已暂停");
        }
    }
}

void MusicPlayer::Resume() {
    if (is_playing_ && !is_paused_) {
        ESP_LOGI(TAG, "Resuming music playback");
        is_paused_ = false;
        
        // 显示继续状态
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            std::string lyric = GetCurrentLyric();
            if (!lyric.empty()) {
                display->SetChatMessage("music", lyric.c_str());
            } else {
                display->SetChatMessage("music", "音乐继续播放");
            }
        }
    }
}
