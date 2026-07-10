#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include "audio_service.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class Http;

enum class MusicPlayerState {
    STOPPED,
    BUFFERING,
    PLAYING,
    STOPPING,
    ERROR,
};

class MusicPlayer {
public:
    static MusicPlayer& GetInstance();

    MusicPlayerState state() const;
    bool IsActive() const;
    const char* StateName() const;

    std::string Play(AudioService& audio_service, const std::string& url, const std::string& title,
                     std::optional<int> volume);
    std::string Stop(AudioService& audio_service, bool wait);

    const std::string& last_error() const;

private:
    static constexpr const char* kTag = "MusicPlayer";
    static constexpr size_t kRingBufferSize = 64 * 1024;
    static constexpr size_t kHttpReadChunkSize = 2048;
    static constexpr uint32_t kDemuxTaskStackSize = 8192;
    static constexpr int kHttpTimeoutMs = 3000;
    static constexpr int kStopWaitMs = 1000;
    static constexpr int kDecodeSampleRate = 24000;
    static constexpr int kOpusChannels = 1;
    static constexpr int kMaxRedirects = 5;

    MusicPlayer() = default;
    ~MusicPlayer();

    static const char* StateName(MusicPlayerState state);
    static bool IsHttpsUrl(const std::string& url);
    static bool IsRedirectStatus(int status_code);
    static bool HasOggUrlHint(const std::string& url);
    static bool IsUnsupportedPageUrl(const std::string& url);
    static bool IsOggContentType(const std::string& content_type);
    static std::string ExtractHostForLog(const std::string& url);
    static std::string ResolveRedirectUrl(const std::string& base_url, const std::string& location);

    bool HasRunningTasksLocked() const;
    bool EnsureRingBufferLocked();
    void FreeRingBufferLocked();
    void ResetRingLocked();
    void CloseRingLocked();
    bool WriteRing(const uint8_t* data, size_t len);
    int ReadRing(uint8_t* data, size_t len);
    bool ValidateOpusHead(const uint8_t* data, size_t len);
    void SetStateLocked(MusicPlayerState state);
    void SetState(MusicPlayerState state);
    void SetError(const std::string& error);
    bool WaitForInactive(TickType_t ticks);

    static void HttpTaskEntry(void* arg);
    static void DemuxTaskEntry(void* arg);
    void HttpTask();
    bool OpenHttpStream(std::unique_ptr<Http>& http, std::string& url);
    void ConfigureHttp(Http& http);
    void ClearHttp(Http* http);
    void MarkTaskFinished(bool http_task);
    void DemuxTask();

    mutable std::mutex mutex_;
    std::condition_variable ring_cv_;
    std::condition_variable state_cv_;
    std::atomic<MusicPlayerState> state_{MusicPlayerState::STOPPED};
    AudioService* audio_service_ = nullptr;
    Http* current_http_ = nullptr;
    TaskHandle_t http_task_handle_ = nullptr;
    TaskHandle_t demux_task_handle_ = nullptr;
    uint8_t* ring_buffer_ = nullptr;
    size_t ring_read_ = 0;
    size_t ring_write_ = 0;
    size_t ring_size_ = 0;
    bool ring_closed_ = false;
    std::atomic<bool> stop_requested_{false};
    bool http_task_running_ = false;
    bool demux_task_running_ = false;
    bool opus_head_validated_ = false;
    std::vector<uint8_t> header_scan_;
    std::string url_;
    std::string title_;
    std::string last_error_;
};

#endif
