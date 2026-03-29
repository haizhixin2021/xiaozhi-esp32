#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include "audio_service.h"
#include "esp_audio_dec_default.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"

// 歌词行结构
struct LyricLine {
    int time_ms;
    std::string text;
};

class MusicPlayer {
public:
    MusicPlayer(AudioService& audio_service);
    ~MusicPlayer();
    
    // 开始播放音乐
    bool Download(const std::string& song_name, const std::string& singer = "");
    
    // 停止播放
    void StopStreaming();
    
    // 暂停播放
    void Pause();
    
    // 继续播放
    void Resume();
    
    // 获取当前播放状态
    bool IsPlaying() const { return is_playing_; }
    bool IsPaused() const { return is_paused_; }
    
    // 获取当前歌词
    std::string GetCurrentLyric() const;
    
private:
    // 下载任务
    void DownloadTask();
    
    // 解码任务
    void DecodeTask();
    
    // 歌词同步任务
    void LyricsSyncTask();
    
    // 搜索音乐
    bool SearchMusic(const std::string& song_name, const std::string& singer, std::string& song_id);
    bool GetSongUrl(const std::string& song_id, std::string& song_url);
    bool GetLyric(const std::string& song_id, std::vector<LyricLine>& lyrics);
    
    // 发送MCP tools/call请求
    std::string SendMcpToolRequest(const std::string& tool_name, const std::string& arguments);
    
    // 解析歌词
    void ParseLyrics(const std::string& lrc_content, std::vector<LyricLine>& lyrics);
    
    // 获取指定时间的歌词
    std::string GetLyricAtTime(const std::vector<LyricLine>& lyrics, int current_time_ms);
    
    // 成员变量
    AudioService& audio_service_;
    
    // 播放状态
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_stopped_;
    std::atomic<bool> is_paused_;
    
    // 任务句柄
    TaskHandle_t download_task_handle_;
    TaskHandle_t decode_task_handle_;
    TaskHandle_t lyrics_sync_task_handle_;
    
    // 事件组
    EventGroupHandle_t event_group_;
    
    // 缓冲区
    std::vector<uint8_t> audio_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    
    // 歌词
    std::vector<LyricLine> lyrics_;
    std::string current_lyric_;
    mutable std::mutex lyric_mutex_;
    
    // 当前歌曲信息
    std::string current_song_name_;
    std::string current_singer_;
    std::string current_song_id_;
    std::string current_song_url_;
    std::string current_lyric_url_;
    
    // 播放时间
    std::atomic<int> current_time_ms_;
    
    // MP3解码器
    esp_audio_simple_dec_handle_t mp3_decoder_ = nullptr;
    int decoder_sample_rate_ = 44100;
    int decoder_channels_ = 2;
    
    // MCP服务地址
    static constexpr const char* MCP_SERVICE_URL = "https://music-mcp.880219.xyz:57860/mcp";
    // MCP服务认证令牌
    static constexpr const char* MCP_AUTH_TOKEN = "44bab3db2734ea21b73cdd5445a93b8e762d77db7a9cbdcce9e1b52d741fa55f";
};

#endif // MUSIC_PLAYER_H
