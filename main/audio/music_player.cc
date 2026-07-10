#include "music_player.h"

#include "board.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <http.h>

namespace {
void LogStackHighWater(const char* tag, const char* point) {
    auto high_water = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(tag, "[MUSIC] music_demux stack_high_water=%u bytes at %s",
             static_cast<unsigned>(high_water), point);
}
}  // namespace

MusicPlayer& MusicPlayer::GetInstance() {
    static MusicPlayer instance;
    return instance;
}

MusicPlayer::~MusicPlayer() {
    std::lock_guard<std::mutex> lock(mutex_);
    FreeRingBufferLocked();
}

MusicPlayerState MusicPlayer::state() const {
    return state_.load();
}

bool MusicPlayer::IsActive() const {
    auto state = state_.load();
    return state == MusicPlayerState::BUFFERING || state == MusicPlayerState::PLAYING ||
           state == MusicPlayerState::STOPPING;
}

const char* MusicPlayer::StateName() const {
    return StateName(state_.load());
}

std::string MusicPlayer::Play(AudioService& audio_service, const std::string& url,
                              const std::string& title, std::optional<int> volume) {
    if (!IsHttpsUrl(url)) {
        return "Loi: self.music.play chi ho tro URL HTTPS truc tiep";
    }
    if (IsUnsupportedPageUrl(url)) {
        return "Loi: khong ho tro URL trang YouTube";
    }

    ESP_LOGI(kTag, "[MUSIC] title=%s", title.c_str());
    ESP_LOGI(kTag, "[MUSIC] volume=%d", volume.value_or(70));
    ESP_LOGI(kTag, "[MUSIC] host=%s", ExtractHostForLog(url).c_str());

    Stop(audio_service, true);
    if (!WaitForInactive(pdMS_TO_TICKS(kStopWaitMs))) {
        return "Loi: trinh phat nhac van dang dung, vui long thu lai";
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_service_ = &audio_service;
        url_ = url;
        title_ = title;
        stop_requested_ = false;
        http_task_running_ = false;
        demux_task_running_ = false;
        header_scan_.clear();
        opus_head_validated_ = false;
        last_error_.clear();
        if (!EnsureRingBufferLocked()) {
            SetStateLocked(MusicPlayerState::ERROR);
            return "Loi: khong cap phat duoc ring buffer PSRAM cho nhac";
        }
        ResetRingLocked();
        SetStateLocked(MusicPlayerState::BUFFERING);
    }

    audio_service.EnableVoiceProcessing(false);
    audio_service.ResetDecoder();
    audio_service.EnableWakeWordDetection(audio_service.IsAfeWakeWord());

    if (volume.has_value()) {
        Board::GetInstance().GetAudioCodec()->SetOutputVolume(*volume);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        http_task_running_ = true;
        demux_task_running_ = true;
    }
    if (xTaskCreate(&MusicPlayer::HttpTaskEntry, "music_http", 4096, this, 4,
                    &http_task_handle_) != pdPASS) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            http_task_running_ = false;
            demux_task_running_ = false;
        }
        SetError("khong tao duoc task HTTP");
        return "Loi: khong tao duoc task HTTP phat nhac";
    }

    if (xTaskCreate(&MusicPlayer::DemuxTaskEntry, "music_demux", kDemuxTaskStackSize, this, 3,
                    &demux_task_handle_) != pdPASS) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            demux_task_running_ = false;
        }
        SetError("khong tao duoc task demux");
        return "Loi: khong tao duoc task demux nhac";
    }
    if (state_.load() == MusicPlayerState::ERROR) {
        std::lock_guard<std::mutex> lock(mutex_);
        return "Loi: " + last_error_;
    }

    std::string response = "Dang phat nhac";
    if (!title.empty()) {
        response += ": " + title;
    }
    return response;
}

std::string MusicPlayer::Stop(AudioService& audio_service, bool wait) {
    bool had_music = false;
    ESP_LOGI(kTag, "[MUSIC] stop requested");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        had_music = IsActive() || HasRunningTasksLocked();
        if (!had_music) {
            ESP_LOGI(kTag, "[MUSIC] state=STOPPED");
            return "Khong co nhac dang phat";
        }
        stop_requested_ = true;
        SetStateLocked(MusicPlayerState::STOPPING);
        CloseRingLocked();
        if (current_http_ != nullptr) {
            current_http_->Close();
        }
    }

    ring_cv_.notify_all();
    audio_service.ResetDecoder();

    if (wait) {
        WaitForInactive(pdMS_TO_TICKS(kStopWaitMs));
    }
    return "Da dung nhac";
}

const std::string& MusicPlayer::last_error() const {
    return last_error_;
}

const char* MusicPlayer::StateName(MusicPlayerState state) {
    switch (state) {
        case MusicPlayerState::STOPPED:
            return "STOPPED";
        case MusicPlayerState::BUFFERING:
            return "BUFFERING";
        case MusicPlayerState::PLAYING:
            return "PLAYING";
        case MusicPlayerState::STOPPING:
            return "STOPPING";
        case MusicPlayerState::ERROR:
            return "ERROR";
    }
    return "UNKNOWN";
}

bool MusicPlayer::IsHttpsUrl(const std::string& url) {
    if (url.size() < 8) {
        return false;
    }
    std::string prefix = url.substr(0, 8);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return prefix == "https://";
}

bool MusicPlayer::IsRedirectStatus(int status_code) {
    return status_code == 301 || status_code == 302 || status_code == 307 || status_code == 308;
}

bool MusicPlayer::HasOggUrlHint(const std::string& url) {
    auto lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find(".ogg") != std::string::npos || lower.find(".opus") != std::string::npos;
}

bool MusicPlayer::IsUnsupportedPageUrl(const std::string& url) {
    auto lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("youtube.com/") != std::string::npos ||
           lower.find("youtu.be/") != std::string::npos;
}

bool MusicPlayer::IsOggContentType(const std::string& content_type) {
    auto lower = content_type;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("audio/ogg") != std::string::npos ||
           lower.find("application/ogg") != std::string::npos ||
           lower.find("audio/opus") != std::string::npos;
}

std::string MusicPlayer::ExtractHostForLog(const std::string& url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return "";
    }

    auto host_start = scheme_end + 3;
    auto host_end = url.find_first_of("/?#", host_start);
    if (host_end == std::string::npos) {
        host_end = url.size();
    }
    return url.substr(host_start, host_end - host_start);
}

std::string MusicPlayer::ResolveRedirectUrl(const std::string& base_url,
                                            const std::string& location) {
    if (location.find("http://") == 0 || location.find("https://") == 0) {
        return location;
    }

    auto scheme_end = base_url.find("://");
    if (scheme_end == std::string::npos) {
        return "";
    }
    auto authority_start = scheme_end + 3;
    auto path_start = base_url.find('/', authority_start);
    std::string origin = path_start == std::string::npos ? base_url : base_url.substr(0, path_start);

    if (!location.empty() && location[0] == '/') {
        return origin + location;
    }

    std::string base_dir = path_start == std::string::npos ? origin + "/" : base_url.substr(0, base_url.rfind('/') + 1);
    return base_dir + location;
}

bool MusicPlayer::HasRunningTasksLocked() const {
    return http_task_running_ || demux_task_running_;
}

bool MusicPlayer::EnsureRingBufferLocked() {
    if (ring_buffer_ != nullptr) {
        return true;
    }
    ring_buffer_ = static_cast<uint8_t*>(
        heap_caps_malloc(kRingBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ring_buffer_ != nullptr;
}

void MusicPlayer::FreeRingBufferLocked() {
    if (ring_buffer_ != nullptr) {
        heap_caps_free(ring_buffer_);
        ring_buffer_ = nullptr;
    }
    ring_read_ = 0;
    ring_write_ = 0;
    ring_size_ = 0;
    ring_closed_ = true;
}

void MusicPlayer::ResetRingLocked() {
    ring_read_ = 0;
    ring_write_ = 0;
    ring_size_ = 0;
    ring_closed_ = false;
}

void MusicPlayer::CloseRingLocked() {
    ring_closed_ = true;
    ring_size_ = 0;
}

bool MusicPlayer::WriteRing(const uint8_t* data, size_t len) {
    size_t written = 0;
    while (written < len) {
        std::unique_lock<std::mutex> lock(mutex_);
        ring_cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
            return stop_requested_ || ring_closed_ || ring_size_ < kRingBufferSize;
        });
        if (stop_requested_ || ring_closed_) {
            return false;
        }

        size_t free_size = kRingBufferSize - ring_size_;
        if (free_size == 0) {
            continue;
        }
        size_t contiguous = std::min(free_size, kRingBufferSize - ring_write_);
        size_t to_copy = std::min(contiguous, len - written);
        std::memcpy(ring_buffer_ + ring_write_, data + written, to_copy);
        ring_write_ = (ring_write_ + to_copy) % kRingBufferSize;
        ring_size_ += to_copy;
        written += to_copy;
        lock.unlock();
        ring_cv_.notify_all();
    }
    return true;
}

int MusicPlayer::ReadRing(uint8_t* data, size_t len) {
    std::unique_lock<std::mutex> lock(mutex_);
    ring_cv_.wait(lock, [this]() { return stop_requested_ || ring_closed_ || ring_size_ > 0; });
    if (stop_requested_) {
        return -1;
    }
    if (ring_size_ == 0 && ring_closed_) {
        return 0;
    }

    size_t contiguous = std::min(ring_size_, kRingBufferSize - ring_read_);
    size_t to_copy = std::min(contiguous, len);
    std::memcpy(data, ring_buffer_ + ring_read_, to_copy);
    ring_read_ = (ring_read_ + to_copy) % kRingBufferSize;
    ring_size_ -= to_copy;
    lock.unlock();
    ring_cv_.notify_all();
    return static_cast<int>(to_copy);
}

bool MusicPlayer::ValidateOpusHead(const uint8_t* data, size_t len) {
    if (opus_head_validated_) {
        return true;
    }

    size_t old_size = header_scan_.size();
    header_scan_.insert(header_scan_.end(), data, data + len);
    if (header_scan_.size() > 4096) {
        header_scan_.erase(header_scan_.begin(), header_scan_.begin() + (header_scan_.size() - 4096));
    }

    const char ogg_sync[] = "OggS";
    auto ogg_it = std::search(header_scan_.begin(), header_scan_.end(), ogg_sync,
                              ogg_sync + sizeof(ogg_sync) - 1);
    if (ogg_it == header_scan_.end()) {
        if (old_size + len >= 4096) {
            SetError("khong tim thay OggS trong stream");
            return false;
        }
        return true;
    }
    size_t ogg_offset = static_cast<size_t>(ogg_it - header_scan_.begin());
    if (header_scan_.size() > ogg_offset + 4 && header_scan_[ogg_offset + 4] != 0) {
        SetError("Ogg stream khong hop le");
        return false;
    }

    const char opus_head[] = "OpusHead";
    auto it = std::search(header_scan_.begin(), header_scan_.end(), opus_head,
                          opus_head + sizeof(opus_head) - 1);
    if (it == header_scan_.end()) {
        if (old_size + len >= 4096) {
            SetError("khong tim thay OpusHead trong Ogg stream");
            return false;
        }
        return true;
    }

    size_t offset = static_cast<size_t>(it - header_scan_.begin());
    if (header_scan_.size() < offset + 19) {
        return true;
    }

    int channels = header_scan_[offset + 9];
    int input_rate = header_scan_[offset + 12] | (header_scan_[offset + 13] << 8) |
                     (header_scan_[offset + 14] << 16) | (header_scan_[offset + 15] << 24);
    if (channels != kOpusChannels) {
        SetError("chi ho tro Ogg Opus mono");
        return false;
    }

    opus_head_validated_ = true;
    ESP_LOGI(kTag, "[MUSIC] opus_channels=%d", channels);
    ESP_LOGI(kTag, "[MUSIC] ogg_opus_validated input=%d Hz, channels=%d, decode=%d Hz",
             input_rate, channels, kDecodeSampleRate);
    return true;
}

void MusicPlayer::SetStateLocked(MusicPlayerState state) {
    state_.store(state);
    state_cv_.notify_all();
    ESP_LOGI(kTag, "[MUSIC] state=%s", StateName(state));
}

void MusicPlayer::SetState(MusicPlayerState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    SetStateLocked(state);
}

void MusicPlayer::SetError(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = error;
        stop_requested_ = true;
        CloseRingLocked();
        if (current_http_ != nullptr) {
            current_http_->Close();
        }
        if (!HasRunningTasksLocked()) {
            FreeRingBufferLocked();
        }
        SetStateLocked(MusicPlayerState::ERROR);
    }
    ring_cv_.notify_all();
    ESP_LOGE(kTag, "[MUSIC] error=%s", error.c_str());
}

bool MusicPlayer::WaitForInactive(TickType_t ticks) {
    std::unique_lock<std::mutex> lock(mutex_);
    return state_cv_.wait_for(lock, std::chrono::milliseconds(ticks * portTICK_PERIOD_MS),
                              [this]() { return !IsActive() && !HasRunningTasksLocked(); });
}

void MusicPlayer::HttpTaskEntry(void* arg) {
    static_cast<MusicPlayer*>(arg)->HttpTask();
    vTaskDelete(nullptr);
}

void MusicPlayer::DemuxTaskEntry(void* arg) {
    static_cast<MusicPlayer*>(arg)->DemuxTask();
    vTaskDelete(nullptr);
}

void MusicPlayer::HttpTask() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = url_;
    }

    std::unique_ptr<Http> http;
    if (OpenHttpStream(http, url)) {
        std::vector<uint8_t> chunk(kHttpReadChunkSize);
        while (!stop_requested_) {
            int ret = http->Read(reinterpret_cast<char*>(chunk.data()), chunk.size());
            if (ret < 0) {
                if (!stop_requested_) {
                    SetError("loi doc HTTPS stream hoac timeout");
                }
                break;
            }
            if (ret == 0) {
                break;
            }
            if (!WriteRing(chunk.data(), static_cast<size_t>(ret))) {
                break;
            }
        }
    }

    ClearHttp(http.get());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ring_closed_ = true;
    }
    ring_cv_.notify_all();
    MarkTaskFinished(true);
}

bool MusicPlayer::OpenHttpStream(std::unique_ptr<Http>& http, std::string& url) {
    for (int redirect = 0; redirect <= kMaxRedirects && !stop_requested_; ++redirect) {
        http = Board::GetInstance().GetNetwork()->CreateHttp(4);
        ConfigureHttp(*http);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_http_ = http.get();
        }

        if (!http->Open("GET", url)) {
            if (!stop_requested_) {
                SetError("khong mo duoc HTTPS stream");
            }
            return false;
        }

        int status_code = http->GetStatusCode();
        if (IsRedirectStatus(status_code)) {
            std::string location = http->GetResponseHeader("Location");
            if (location.empty()) {
                location = http->GetResponseHeader("location");
            }
            if (location.empty()) {
                SetError("HTTP redirect thieu Location");
                return false;
            }
            if (redirect == kMaxRedirects) {
                SetError("HTTP redirect qua nhieu lan");
                return false;
            }

            std::string next_url = ResolveRedirectUrl(url, location);
            if (!IsHttpsUrl(next_url)) {
                SetError("HTTP redirect khong phai HTTPS");
                return false;
            }
            ESP_LOGI(kTag, "HTTP redirect %d -> host=%s", status_code,
                     ExtractHostForLog(next_url).c_str());
            ClearHttp(http.get());
            http.reset();
            url = next_url;
            continue;
        }

        ESP_LOGI(kTag, "[MUSIC] http_status=%d", status_code);
        if (status_code != 200) {
            SetError("HTTP status khong hop le: " + std::to_string(status_code));
            return false;
        }

        std::string content_type = http->GetResponseHeader("Content-Type");
        if (content_type.empty()) {
            content_type = http->GetResponseHeader("content-type");
        }
        ESP_LOGI(kTag, "[MUSIC] content_type=%s", content_type.c_str());
        if (!content_type.empty() && !IsOggContentType(content_type) && !HasOggUrlHint(url)) {
            SetError("URL khong phai Ogg/Opus truc tiep");
            return false;
        }
        return true;
    }

    return false;
}

void MusicPlayer::ConfigureHttp(Http& http) {
    http.SetTimeout(kHttpTimeoutMs);
    http.SetHeader("Accept", "audio/ogg, application/ogg, audio/opus");
    http.SetKeepAlive(false);
}

void MusicPlayer::ClearHttp(Http* http) {
    if (http != nullptr) {
        http->Close();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_http_ == http) {
        current_http_ = nullptr;
    }
}

void MusicPlayer::MarkTaskFinished(bool http_task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (http_task) {
        http_task_running_ = false;
        http_task_handle_ = nullptr;
    } else {
        demux_task_running_ = false;
        demux_task_handle_ = nullptr;
    }

    if (!HasRunningTasksLocked()) {
        current_http_ = nullptr;
        FreeRingBufferLocked();
        audio_service_ = nullptr;
        url_.clear();
        title_.clear();
        header_scan_.clear();
        opus_head_validated_ = false;
        if (state_.load() != MusicPlayerState::ERROR) {
            SetStateLocked(MusicPlayerState::STOPPED);
        }
    }
    state_cv_.notify_all();
}

void MusicPlayer::DemuxTask() {
    LogStackHighWater(kTag, "task_start");

    auto demuxer = std::make_unique<OggDemuxer>();
    LogStackHighWater(kTag, "after_ogg_demuxer_create");
    if (!demuxer->IsReady()) {
        SetError("khong cap phat duoc OggDemuxer packet buffer");
        LogStackHighWater(kTag, "before_task_exit");
        MarkTaskFinished(false);
        return;
    }

    size_t packet_count = 0;
    demuxer->OnDemuxerFinished([this, &packet_count](const uint8_t* data, int sample_rate, size_t size) {
        (void)sample_rate;
        if (stop_requested_) {
            return;
        }
        if (!opus_head_validated_) {
            SetError("Ogg stream chua duoc validate OpusHead");
            return;
        }
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = kDecodeSampleRate;
        packet->frame_duration = OPUS_FRAME_DURATION_MS;
        packet->payload.assign(data, data + size);
        if (audio_service_ != nullptr) {
            while (!stop_requested_) {
                if (audio_service_->PushPacketToDecodeQueue(
                        std::make_unique<AudioStreamPacket>(*packet), false)) {
                    ++packet_count;
                    if (packet_count == 1) {
                        LogStackHighWater(kTag, "after_packet_1");
                    } else if (packet_count == 2) {
                        LogStackHighWater(kTag, "after_packet_2");
                    } else if (packet_count == 3) {
                        LogStackHighWater(kTag, "after_packet_3");
                    } else if ((packet_count % 64) == 0) {
                        LogStackHighWater(kTag, "periodic_playback");
                    }
                    if (state_.load() == MusicPlayerState::BUFFERING) {
                        SetState(MusicPlayerState::PLAYING);
                    }
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    });
    demuxer->Reset();

    auto chunk = std::unique_ptr<uint8_t, decltype(&heap_caps_free)>(
        static_cast<uint8_t*>(
            heap_caps_malloc(kHttpReadChunkSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        heap_caps_free);
    if (!chunk) {
        chunk.reset(static_cast<uint8_t*>(
            heap_caps_malloc(kHttpReadChunkSize, MALLOC_CAP_8BIT)));
    }
    if (!chunk) {
        SetError("khong cap phat duoc demux read buffer");
        LogStackHighWater(kTag, "before_task_exit");
        MarkTaskFinished(false);
        return;
    }

    bool header_logged = false;
    while (true) {
        int ret = ReadRing(chunk.get(), kHttpReadChunkSize);
        if (ret < 0) {
            break;
        }
        if (ret == 0) {
            if (!stop_requested_ && audio_service_ != nullptr) {
                audio_service_->WaitForPlaybackQueueEmpty();
            }
            break;
        }
        if (!ValidateOpusHead(chunk.get(), static_cast<size_t>(ret))) {
            break;
        }
        if (!header_logged && opus_head_validated_) {
            header_logged = true;
            LogStackHighWater(kTag, "after_ogg_opus_header");
        }
        demuxer->Process(chunk.get(), static_cast<size_t>(ret));
    }

    LogStackHighWater(kTag, "before_task_exit");
    MarkTaskFinished(false);
}
