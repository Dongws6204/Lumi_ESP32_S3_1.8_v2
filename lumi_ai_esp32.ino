/*
  Lumi-AI — Waveshare ESP32-S3-Touch-AMOLED-1.8

  Hands-free flow: VAD detects speech -> streams PCM chunks over WebSocket.
  The FastAPI backend sends LED commands and 24 kHz PCM TTS over that same socket.
  The loop has no recording heap allocation, HTTP upload task, or cross-core upload state.
*/
#include <Arduino.h> // Thư viện lõi của Arduino dành cho ESP32
#include "config.h"  // Hardware, network, and deployment configuration
#include <WiFi.h>    // Thư viện quản lý kết nối Wi-Fi
#if LUMI_USE_TLS
#include <WiFiClientSecure.h>
#endif
#include <Wire.h>              // Thư viện giao tiếp I2C
#include <WebSocketsClient.h>  // Thư viện kết nối WebSocket Client đến backend
#include <ArduinoJson.h>       // Thư viện mã hóa/giải mã dữ liệu JSON
#include <Adafruit_NeoPixel.h> // Thư viện điều khiển dải LED RGB/RGBW
#include <ESP_I2S.h>           // Thư viện chính thức của Espressif để cấu hình giao tiếp âm thanh I2S
#include <Preferences.h>       // Lưu cấu hình người dùng vào NVS của ESP32
#include <freertos/FreeRTOS.h>

namespace
{
  // Các hằng số cấu hình nội bộ
  // Brightness is kept in the same 0-100 scale used by the server protocol.
  // The SK6812 library uses 0-255, therefore conversion happens only at render.
  static constexpr uint8_t kBrightnessDefault = 65;
  static constexpr uint8_t kBrightnessMax = 100;        // User-facing protocol scale.
  static constexpr uint8_t kPhysicalBrightnessMax = 80; // Electrical/power safety cap.
  static constexpr uint8_t kBrightnessStep = 10;
  constexpr uint8_t kVolumeDefault = 85;          // Âm lượng loa mặc định (0-100%)
  constexpr uint8_t kVolumeProfileVersion = 2;    // Migrates the previous 95% default to 85% once.
  constexpr uint16_t kEs8311Address0 = 0x18;      // Địa chỉ I2C cấu hình thứ nhất của chip âm thanh ES8311
  constexpr uint16_t kEs8311Address1 = 0x19;      // Địa chỉ I2C cấu hình thứ hai của chip âm thanh ES8311
  constexpr uint32_t kHeartbeatMs = 30000;        // Chu kỳ gửi bản tin Heartbeat giữ kết nối (30 giây)
  constexpr uint32_t kWifiRetryMs = 10000;        // Khoảng thời gian thử kết nối lại WiFi khi mất mạng (10 giây)
  constexpr size_t kMicFramesPerTick = 128;       // Số mẫu micro đọc mỗi lượt (128 mẫu tương đương 8ms ở tần số 16kHz)
  constexpr size_t kAudioStreamChunkBytes = 2048; // 64ms PCM16 mono at 16 kHz
  constexpr size_t kAudioStreamChunkSamples = kAudioStreamChunkBytes / sizeof(int16_t);
  constexpr size_t kMaxServerJsonBytes = 1024;
  constexpr size_t kPlaybackBufferBytes = 16384;
  constexpr size_t kPlaybackConvertSamples = 1024;
  // A larger DMA submission gives the loop enough scheduling slack while the
  // WebSocket and JSON work run between playback service calls.
  constexpr size_t kPlaybackWriteChunkBytes = 4096;
  constexpr uint32_t kSettingsSaveDelayMs = 2000;
  constexpr uint32_t kErrorRecoveryMs = 5000;
  constexpr uint32_t kProcessingTimeoutMs = 15000; // Fallback if server never replies (Gemini timeout)
  constexpr uint32_t kInterruptDebounceMs = 250;

  // Định nghĩa các trạng thái hoạt động của trợ lý ảo AI
  enum class AiState
  {
    BOOTING,
    WIFI_CONNECTING,
    WS_CONNECTING,
    IDLE,
    LISTENING,
    RECORDING,
    PROCESSING,
    SPEAKING,
    ERROR
  };

  /*
     Lớp điều khiển chip giải mã âm thanh ES8311 qua I2C.
     Tránh sử dụng thư viện cồng kềnh, cấu hình trực tiếp các thanh ghi theo tài liệu nhà sản xuất.
  */
  class Es8311Codec
  {
  public:
    // Khởi tạo codec âm thanh ES8311
    bool begin(TwoWire &wire)
    {
      wire_ = &wire;
      // Dò tìm thiết bị ES8311 trên bus I2C ở địa chỉ 0x18 hoặc 0x19
      address_ = probe(kEs8311Address0) ? kEs8311Address0 : (probe(kEs8311Address1) ? kEs8311Address1 : 0);
      if (!address_)
        return false; // Trả về false nếu không tìm thấy chip ES8311

      return initialize();
    }

    // Restore a known codec register state after a failed clock change.  This
    // keeps the ES8311 from retaining a partial clock plan while I2S is reset.
    bool reset()
    {
      return wire_ && address_ && initialize();
    }

    // Thiết lập tần số lấy mẫu (Sample Rate) cho chip codec
    bool setSampleRate(uint32_t sampleRate)
    {
      struct ClockConfig
      {
        uint32_t mclkHz;
        uint8_t reg02;
        uint8_t reg03;
        uint8_t reg04;
        uint8_t reg05;
        uint8_t reg06;
        uint8_t reg07;
        uint8_t reg08;
      };
      // ESP_I2S uses a 256×Fs MCLK. The official ES8311 coefficient table has
      // matching divider values for both of these 16-bit stereo configurations.
      // Keep separate entries so a later MCLK/format change cannot silently reuse
      // an incompatible clock plan.
      static constexpr ClockConfig kClock16k = {4096000, 0x00, 0x10, 0x10, 0x00, 0x04, 0x00, 0xFF};
      static constexpr ClockConfig kClock24k = {6144000, 0x00, 0x10, 0x10, 0x00, 0x04, 0x00, 0xFF};
      const ClockConfig *config = nullptr;
      if (sampleRate == LUMI_AUDIO_SAMPLE_RATE)
        config = &kClock16k;
      else if (sampleRate == LUMI_TTS_SAMPLE_RATE)
        config = &kClock24k;
      else
        return false;
      if (config->mclkHz != sampleRate * 256UL)
        return false;

      return write(0x02, config->reg02) && write(0x03, config->reg03) && write(0x04, config->reg04) &&
             write(0x05, config->reg05) && write(0x06, config->reg06) && write(0x07, config->reg07) &&
             write(0x08, config->reg08);
    }

    // Cài đặt âm lượng loa (tính theo phần trăm 0 - 100)
    bool setVolume(uint8_t percent)
    {
      // Chuyển đổi từ thang đo 0-100% sang giá trị thanh ghi âm lượng 8-bit (0-255)
      const uint8_t clamped = min(percent, static_cast<uint8_t>(100));
      const uint16_t scaled = static_cast<uint16_t>(clamped) * 256U / 100U;
      const uint8_t level = clamped == 0 ? 0 : static_cast<uint8_t>(scaled - 1U);
      return write(0x32, level); // Ghi vào thanh ghi điều khiển âm lượng của ES8311
    }

    // Cài đặt độ lợi (Gain) cho Micro (tính bằng dB)
    bool setMicGain(uint8_t db)
    {
      // Tính toán giá trị tương ứng để ghi vào thanh ghi điều khiển độ lợi micro
      const uint8_t gain = db <= 42 ? static_cast<uint8_t>(db / 6 + 1) : 8;
      return write(0x16, gain); // Thanh ghi 0x16 quản lý độ lợi đầu vào của Micro
    }

  private:
    bool initialize()
    {
      // Reset chip và khởi động các khối tương tự ADC/DAC trước khi bắt đầu I2S.
      return write(0x00, 0x1F) && write(0x00, 0x00) && write(0x00, 0x80) &&
             write(0x01, 0x3F) &&                      // Cấu hình xung clock cho ES8311 lấy từ chân MCLK GPIO
             write(0x09, 0x0C) && write(0x0A, 0x0C) && // Đặt định dạng I2S đầu vào và đầu ra là 16-bit
             write(0x0D, 0x01) && write(0x0E, 0x02) &&
             write(0x12, 0x00) && write(0x13, 0x10) &&
             write(0x14, 0x1A) && write(0x17, 0xC8) && write(0x1C, 0x6A) &&
             write(0x37, 0x08) && setMicGain(18) && setVolume(kVolumeDefault); // lưu ý có thể nâng lên 24 vì 18 đến 14 là khoảng vàng.
    }
    // Kiểm tra xem thiết bị I2C có phản hồi tại địa chỉ truyền vào hay không
    bool probe(uint8_t address)
    {
      wire_->beginTransmission(address);
      return wire_->endTransmission() == 0; // Trả về true nếu nhận được tín hiệu ACK
    }

    // Ghi một giá trị byte vào thanh ghi cụ thể của ES8311 qua bus I2C
    bool write(uint8_t reg, uint8_t value)
    {
      wire_->beginTransmission(address_);
      wire_->write(reg);                    // Gửi địa chỉ thanh ghi cần ghi
      wire_->write(value);                  // Gửi giá trị cần ghi vào thanh ghi
      return wire_->endTransmission() == 0; // Trả về true nếu truyền dữ liệu thành công
    }
    TwoWire *wire_ = nullptr; // Con trỏ lưu đối tượng Wire giao tiếp I2C
    uint8_t address_ = 0;     // Địa chỉ I2C thực tế được phát hiện của chip ES8311
  };

  // Khai báo các đối tượng ngoại vi toàn cục
  WebSocketsClient webSocket;                                                        // Đối tượng khách kết nối WebSocket
  I2SClass i2s;                                                                      // Đối tượng I2S để truyền/nhận âm thanh số
  Es8311Codec codec;                                                                 // Đối tượng cấu hình codec ES8311
  Preferences preferences;                                                           // NVS cho brightness và volume
  Adafruit_NeoPixel strip(LUMI_LED_COUNT, LUMI_LED_DATA_PIN, NEO_GRBW + NEO_KHZ800); // Đối tượng điều khiển LED RGBW

  // The main loop is the sole owner of I2S, WebSocket, playback and capture state.

  // Biến trạng thái sẵn sàng và các biến điều khiển của hệ thống
  bool ledReady = false;                              // Đèn LED đã được khởi tạo thành công hay chưa
  bool webSocketConnected = false;                    // Kết nối WebSocket tới server đã được thiết lập hay chưa
  bool i2sReady = false;                              // Cổng I2S đã được khởi tạo thành công hay chưa
  uint32_t activeSampleRate = 0;                      // Tần số I2S hiện tại
  bool codecReady = false;                            // Chip giải mã âm thanh ES8311 đã hoạt động hay chưa
  bool recording = false;                             // Đang trong quá trình ghi âm giọng nói hay không
  bool playbackActive = false;                        // Đang trong trạng thái phát âm thanh phản hồi từ server hay không
  bool playbackEndRequested = false;                  // Server has sent AUDIO_RESPONSE_END; drain queued PCM first
  bool microphoneEnabled = false;                     // Khóa logic ADC khi không được phép ghi âm
  bool lightPower = true;                             // Trạng thái bật/tắt nguồn sáng của đèn
  uint8_t brightness = kBrightnessDefault;            // Độ sáng hiện tại của đèn
  uint8_t volume = kVolumeDefault;                    // Âm lượng hiện tại của loa
  uint32_t lampColor = 0;                             // Màu sắc đèn hiện tại dưới dạng mã màu RGB
  String colorHex = "#FFF5E0";                        // Chuỗi Hex biểu diễn màu sắc đèn hiện tại
  AiState aiState = AiState::BOOTING;                 // Trạng thái hoạt động hiện tại của hệ thống AI
  int16_t audioStreamChunk[kAudioStreamChunkSamples]; // 2 KB static PCM staging buffer
  size_t audioStreamChunkSamples = 0;
  size_t recordedSamples = 0; // Number of streamed mono PCM samples in this turn
  char audioSessionId[32] = {};
  uint32_t lastVoiceAt = 0;       // Mốc thời gian (ms) cuối cùng phát hiện thấy giọng nói trên ngưỡng lọc nhiễu
  uint32_t lastHeartbeatAt = 0;   // Mốc thời gian (ms) cuối cùng gửi bản tin giữ kết nối Heartbeat
  uint32_t lastWifiAttemptAt = 0; // Mốc thời gian (ms) cuối cùng thực hiện thử kết nối lại WiFi
  uint32_t lastErrorAt = 0;
  uint32_t lastProcessingAt = 0; // Watchdog: exit PROCESSING if server never responds
  uint32_t settingsChangedAt = 0;
  bool settingsDirty = false;
  uint8_t playbackBuffer[kPlaybackBufferBytes];
  int16_t playbackStereoConvertBuffer[kPlaybackConvertSamples * 2];
  size_t playbackReadOffset = 0;
  size_t playbackUsedBytes = 0;
  // I2S stays 16-bit stereo.  Server PCM can be mono or stereo as declared by
  // AUDIO_RESPONSE_START; mono samples are duplicated before they reach I2S.
  uint8_t playbackInputChannels = 1;
  uint32_t lastInterruptAt = 0;
  bool interruptButtonWasPressed = false;

  // User brightness remains 0--100 in NVS and in the WebSocket protocol.
  uint8_t clampBrightness(int value) { return static_cast<uint8_t>(constrain(value, 0, kBrightnessMax)); }
  // Giới hạn giá trị âm lượng trong phạm vi cho phép (0 tới 100)
  uint8_t clampVolume(int value) { return static_cast<uint8_t>(constrain(value, 0, 100)); }

  uint8_t brightnessToStripLevel(uint8_t percent)
  {
    const uint16_t physicalPercent = static_cast<uint16_t>(percent) * kPhysicalBrightnessMax / 100U;
    return static_cast<uint8_t>(physicalPercent * 255U / 100U);
  }

  bool hasNetworkConfiguration()
  {
    return LUMI_WIFI_SSID[0] != '\0' && LUMI_SERVER_HOST[0] != '\0';
  }

  void clearPlaybackBuffer()
  {
    playbackReadOffset = 0;
    playbackUsedBytes = 0;
  }

  void processPlayback();

  bool queuePlaybackAudio(const uint8_t *data, size_t length)
  {
    if (!data || !length || !playbackActive || !i2sReady)
      return false;

    // WebSocket can deliver PCM much faster than I2S can play it.  Do not drop
    // a full frame here: stop consuming WebSocket data until I2S makes room so
    // TCP flow control applies backpressure to the server.
    while (length)
    {
      while (playbackUsedBytes == kPlaybackBufferBytes)
      {
        processPlayback();
        if (playbackUsedBytes == kPlaybackBufferBytes)
          vTaskDelay(1);
        if (!playbackActive || !i2sReady)
          return false;
      }

      const size_t freeBytes = kPlaybackBufferBytes - playbackUsedBytes;
      const size_t bytesToQueue = min(length, freeBytes);
      const size_t writeOffset = (playbackReadOffset + playbackUsedBytes) % kPlaybackBufferBytes;
      const size_t firstPart = min(bytesToQueue, kPlaybackBufferBytes - writeOffset);
      memcpy(playbackBuffer + writeOffset, data, firstPart);
      if (bytesToQueue > firstPart)
        memcpy(playbackBuffer, data + firstPart, bytesToQueue - firstPart);
      playbackUsedBytes += bytesToQueue;
      data += bytesToQueue;
      length -= bytesToQueue;
    }
    return true;
  }

  bool queueMonoPlaybackAudio(const uint8_t *data, size_t length)
  {
    if (length % sizeof(int16_t))
    {
      Serial.printf("[AUDIO] Rejected mono PCM frame with odd byte count: %u\n", static_cast<unsigned>(length));
      return false;
    }

    size_t monoSamples = 0;
    size_t stereoBytes = 0;
    while (length)
    {
      const size_t samples = min(length / sizeof(int16_t), kPlaybackConvertSamples);
      for (size_t i = 0; i < samples; ++i)
      {
        int16_t monoSample;
        memcpy(&monoSample, data + i * sizeof(monoSample), sizeof(monoSample));
        playbackStereoConvertBuffer[i * 2] = monoSample;     // Left
        playbackStereoConvertBuffer[i * 2 + 1] = monoSample; // Right
      }
      const size_t bytes = samples * 2 * sizeof(playbackStereoConvertBuffer[0]);
      if (!queuePlaybackAudio(reinterpret_cast<const uint8_t *>(playbackStereoConvertBuffer), bytes))
        return false;
      data += samples * sizeof(int16_t);
      length -= samples * sizeof(int16_t);
      monoSamples += samples;
      stereoBytes += bytes;
    }
    Serial.printf("[AUDIO] converted %u mono samples -> %u stereo bytes\n",
                  static_cast<unsigned>(monoSamples), static_cast<unsigned>(stereoBytes));
    return true;
  }

  // Hàm chuyển đổi chuỗi mã màu Hex (vd: "#FFF5E0") sang dạng số nguyên RGB 32-bit
  uint32_t parseHexColor(const char *text)
  {
    if (!text || text[0] != '#' || strlen(text) != 7)
      return 0; // Trả về màu đen nếu định dạng chuỗi không hợp lệ
    char *end = nullptr;
    const unsigned long value = strtoul(text + 1, &end, 16); // Chuyển đổi chuỗi Hex cơ số 16 thành số nguyên
    return end && *end == '\0' ? value : 0;                  // Trả về giá trị màu nếu phân tích thành công
  }

  // Chuyển đổi trạng thái hoạt động enum AiState sang dạng chuỗi text để hiển thị/gửi đi
  const char *aiStateToString(AiState state)
  {
    switch (state)
    {
    case AiState::BOOTING:
      return "BOOTING";
    case AiState::WIFI_CONNECTING:
      return "WIFI_CONNECTING";
    case AiState::WS_CONNECTING:
      return "WS_CONNECTING";
    case AiState::LISTENING:
      return "LISTENING";
    case AiState::RECORDING:
      return "RECORDING";
    case AiState::PROCESSING:
      return "PROCESSING";
    case AiState::SPEAKING:
      return "SPEAKING";
    case AiState::ERROR:
      return "ERROR";
    default:
      return "IDLE";
    }
  }

  const char *aiStateName() { return aiStateToString(aiState); }

  // Hàm thiết lập toàn bộ bóng LED hiển thị cùng một màu RGBW cụ thể
  void fillLeds(uint8_t r, uint8_t g, uint8_t b, uint8_t w)
  {
    if (!ledReady)
      return;                                       // Không thực hiện nếu LED chưa được khởi tạo
    const uint32_t color = strip.Color(r, g, b, w); // Tạo mã màu RGBW từ các thành phần đơn lẻ
    for (uint16_t i = 0; i < LUMI_LED_COUNT; ++i)
      strip.setPixelColor(i, color); // Gán màu cho từng bóng LED trong dải
    strip.show();                    // Cập nhật tín hiệu hiển thị vật lý lên dải LED
  }

  // Cập nhật hiệu ứng ánh sáng của đèn LED dựa trên trạng thái hệ thống AI hiện tại
  void renderLamp()
  {
    if (!ledReady)
      return; // Bỏ qua nếu LED chưa sẵn sàng
    if (aiState == AiState::LISTENING || aiState == AiState::RECORDING)
    {
      // Listening/recording: soft warm yellow for the Trúc Chỉ lamp.
      strip.setBrightness(brightnessToStripLevel(60));
      fillLeds(255, 180, 80, 200);
      return;
    }
    if (aiState == AiState::PROCESSING)
    {
      // Processing keeps the same Trúc Chỉ palette, with a steady warm glow.
      strip.setBrightness(brightnessToStripLevel(60));
      fillLeds(255, 180, 80, 200);
      return;
    }
    if (aiState == AiState::SPEAKING)
    {
      // Speaking keeps the same Trúc Chỉ palette; only its animation may vary.
      strip.setBrightness(brightnessToStripLevel(60));
      fillLeds(255, 180, 80, 200);
      return;
    }
    if (aiState == AiState::ERROR)
    {
      // Error: soft orange-red rather than a harsh pure red.
      strip.setBrightness(brightnessToStripLevel(45));
      fillLeds(220, 60, 20, 0);
      return;
    }

    // Nếu ở trạng thái IDLE (chờ bình thường): áp dụng độ sáng do người dùng cấu hình
    strip.setBrightness(brightnessToStripLevel(brightness));
    if (!lightPower)
    {
      // Nếu tắt nguồn đèn: tắt toàn bộ các bóng LED
      fillLeds(0, 0, 0, 0);
      return;
    }
    if (lampColor)
      // Nếu có cấu hình màu tùy chọn: tách các kênh R, G, B để hiển thị màu đó
      fillLeds((lampColor >> 16) & 0xFF, (lampColor >> 8) & 0xFF, lampColor & 0xFF, 0);
    else
      // Mặc định: sáng bóng LED trắng ấm áp chuyên dụng (warm-white die) để làm đèn ngủ/đèn bàn
      fillLeds(0, 0, 0, 255);
  }

  // Hàm cấu hình cổng I2S hoạt động theo tần số lấy mẫu chỉ định
  bool configureI2S(uint32_t sampleRate)
  {
    if (i2sReady)
    {
      i2s.end(); // Giải phóng I2S cũ nếu nó đang chạy trước đó
      i2sReady = false;
    }
    activeSampleRate = 0;
    // Thiết lập sơ đồ chân GPIO kết nối vật lý cho I2S: BCLK, WS, DOUT (tới ES8311), DIN (từ ES8311), MCLK
    i2s.setPins(LUMI_I2S_BCLK, LUMI_I2S_WS, LUMI_I2S_DOUT, LUMI_I2S_DIN, LUMI_I2S_MCLK);
    // Khởi tạo I2S: Chế độ chuẩn (STD), tần số chỉ định, độ rộng mẫu 16-bit, chế độ Stereo (2 kênh), dùng cả 2 slot trái/phải
    i2sReady = i2s.begin(I2S_MODE_STD, sampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                         I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
    if (!i2sReady)
      return false;

    if (codecReady && !codec.setSampleRate(sampleRate))
    {
      Serial.printf("[AUDIO] ES8311 rejected sample rate %lu\n", static_cast<unsigned long>(sampleRate));
      i2s.end();
      i2sReady = false;
      // A failed rate change can leave only some ES8311 clock registers updated.
      // Reset its register plan while the PA is disabled before a later retry.
      digitalWrite(LUMI_ES8311_PA_ENABLE, LOW);
      if (!codec.reset())
      {
        codecReady = false;
        Serial.println("[AUDIO] ES8311 reset failed after sample-rate error");
      }
      return false;
    }
    i2s.setTimeout(0); // Never block the main loop when DMA cannot accept data.
    activeSampleRate = sampleRate;
    return i2sReady;
  }

  // Gửi đối tượng dữ liệu JSON qua kết nối WebSocket tới server
  bool sendJson(JsonDocument &document)
  {
    if (!webSocketConnected)
      return false; // Bỏ qua nếu chưa kết nối thành công tới server
    String text;
    serializeJson(document, text);  // Chuyển đổi đối tượng JSON sang chuỗi văn bản
    return webSocket.sendTXT(text); // Thực hiện gửi chuỗi đi qua kết nối WebSocket
  }

  // Báo cáo đầy đủ trạng thái hiện tại của đèn lên server thông qua WebSocket
  void reportState()
  {
    JsonDocument doc;
    doc["type"] = "STATE_REPORT"; // Đánh dấu loại bản tin báo cáo trạng thái
    JsonObject p = doc["payload"].to<JsonObject>();
    p["light_power"] = lightPower;
    p["brightness"] = brightness;
    p["color"] = colorHex;
    p["volume"] = volume;
    p["is_playing_music"] = false;
    p["led_effect"] = "STEADY";
    p["led_ws_state"] = aiStateName();
    doc["timestamp"] = millis() / 1000; // Đính kèm mốc thời gian chạy (giây) của hệ thống
    sendJson(doc);
  }

  // Gửi gói tin Heartbeat để duy trì phiên kết nối hoạt động với server
  void sendHeartbeat()
  {
    JsonDocument doc;
    doc["type"] = "HEARTBEAT"; // Loại bản tin duy trì kết nối
    JsonObject p = doc["payload"].to<JsonObject>();
    p["firmware_version"] = "0.4.0-ws-audio-stream";
    p["wifi_rssi"] = WiFi.RSSI();       // Đo cường độ tín hiệu sóng WiFi hiện tại (dBm)
    p["free_heap"] = ESP.getFreeHeap(); // Lượng RAM trống còn lại của vi điều khiển ESP32
    p["uptime_ms"] = millis();          // Tổng thời gian từ lúc cấp nguồn chạy thiết bị (ms)
    p["light_power"] = lightPower;
    p["brightness"] = brightness;
    p["volume"] = volume;
    doc["timestamp"] = millis() / 1000;
    sendJson(doc);
    lastHeartbeatAt = millis(); // Cập nhật mốc thời gian gửi Heartbeat gần nhất
  }

  // The ES8311 wrapper exposes volume, gain and sample-rate controls, but not
  // a documented ADC/DAC mute API.  The microphone lock is therefore enforced
  // by this flag and the loop gate; the PA and volume provide the speaker lock.
  void setVolume(uint8_t value)
  {
    if (codecReady)
      codec.setVolume(clampVolume(value));
  }

  void setMicGain(uint8_t value)
  {
    if (codecReady)
      codec.setMicGain(value);
  }

  void muteMicrophone()
  {
    microphoneEnabled = false;
    Serial.println("[AUDIO] Microphone disabled");
  }

  void muteSpeaker()
  {
    setVolume(0);
    digitalWrite(LUMI_ES8311_PA_ENABLE, LOW);
    Serial.println("[AUDIO] Speaker disabled (volume=0, PA=OFF)");
  }

  bool switchToListeningMode()
  {
    playbackActive = false;
    playbackEndRequested = false;
    clearPlaybackBuffer();
    muteSpeaker();
    if ((!i2sReady || activeSampleRate != LUMI_AUDIO_SAMPLE_RATE) && !configureI2S(LUMI_AUDIO_SAMPLE_RATE))
    {
      Serial.println("[AUDIO] Failed to configure 16000 Hz listening mode");
      return false;
    }
    setMicGain(18);
    microphoneEnabled = true;
    Serial.printf("[AUDIO] Listening mode: mic=ON speaker=OFF sampleRate=%lu\n",
                  static_cast<unsigned long>(LUMI_AUDIO_SAMPLE_RATE));
    return true;
  }

  bool switchToProcessingMode()
  {
    recording = false;
    playbackEndRequested = false;
    muteMicrophone();
    muteSpeaker();
    clearPlaybackBuffer();
    Serial.println("[AUDIO] Processing mode: mic=OFF speaker=OFF");
    return true;
  }

  bool switchToSpeakingMode()
  {
    recording = false;
    playbackEndRequested = false;
    muteMicrophone();
    clearPlaybackBuffer();
    if ((!i2sReady || activeSampleRate != LUMI_TTS_SAMPLE_RATE) && !configureI2S(LUMI_TTS_SAMPLE_RATE))
    {
      playbackActive = false;
      muteSpeaker();
      Serial.println("[AUDIO] Failed to configure 24000 Hz speaking mode");
      return false;
    }
    playbackActive = true;
    setVolume(volume);
    digitalWrite(LUMI_ES8311_PA_ENABLE, HIGH);
    Serial.printf("[AUDIO] Speaking mode: mic=OFF speaker=ON sampleRate=%lu volume=%u\n",
                  static_cast<unsigned long>(LUMI_TTS_SAMPLE_RATE), volume);
    return true;
  }

  void enterErrorState()
  {
    aiState = AiState::ERROR;
    recording = false;
    recordedSamples = 0;
    audioStreamChunkSamples = 0;
    audioSessionId[0] = '\0';
    playbackActive = false;
    playbackEndRequested = false;
    clearPlaybackBuffer();
    muteMicrophone();
    muteSpeaker();
    lastErrorAt = millis();
  }

  void setAiState(AiState newState)
  {
    if (aiState == newState)
      return;

    Serial.printf("[AI_STATE] %s -> %s\n", aiStateToString(aiState), aiStateToString(newState));
    aiState = newState;
    bool transitionSucceeded = true;
    switch (aiState)
    {
    case AiState::IDLE:
    case AiState::LISTENING:
    case AiState::RECORDING:
      transitionSucceeded = switchToListeningMode();
      break;
    case AiState::BOOTING:
    case AiState::WIFI_CONNECTING:
    case AiState::WS_CONNECTING:
      recording = false;
      muteMicrophone();
      muteSpeaker();
      break;
    case AiState::PROCESSING:
      transitionSucceeded = switchToProcessingMode();
      lastProcessingAt = millis();
      break;
    case AiState::SPEAKING:
      transitionSucceeded = switchToSpeakingMode();
      break;
    case AiState::ERROR:
      enterErrorState();
      break;
    }
    if (!transitionSucceeded)
    {
      Serial.println("[AI_STATE] audio transition failed -> ERROR");
      enterErrorState();
    }
    renderLamp();
  }

  // Compatibility mapping for server LED_STATE messages used by earlier builds.
  void setAiState(const char *state)
  {
    if (!state)
      return;
    if (!strcmp(state, "BOOTING"))
      setAiState(AiState::BOOTING);
    else if (!strcmp(state, "WIFI_CONNECTING"))
      setAiState(AiState::WIFI_CONNECTING);
    else if (!strcmp(state, "WS_CONNECTING"))
      setAiState(AiState::WS_CONNECTING);
    else if (!strcmp(state, "LISTENING"))
      setAiState(AiState::LISTENING);
    else if (!strcmp(state, "RECORDING"))
      setAiState(AiState::RECORDING);
    else if (!strcmp(state, "THINKING") || !strcmp(state, "PROCESSING"))
      setAiState(AiState::PROCESSING);
    else if (!strcmp(state, "SPEAKING") || !strcmp(state, "PLAYING"))
      setAiState(AiState::SPEAKING);
    else if (!strcmp(state, "ERROR"))
      setAiState(AiState::ERROR);
    else
      setAiState(AiState::IDLE);
  }

  void scheduleSettingsSave()
  {
    settingsDirty = true;
    settingsChangedAt = millis();
  }

  void flushSettingsIfDue()
  {
    if (!settingsDirty || millis() - settingsChangedAt < kSettingsSaveDelayMs)
      return;
    if (!preferences.begin("lumi-ai", false))
    {
      Serial.println("[PREF] Failed to open NVS for writing");
      settingsChangedAt = millis();
      return;
    }
    preferences.putUChar("brightness", brightness);
    preferences.putUChar("volume", volume);
    preferences.end();
    settingsDirty = false;
    Serial.println("[PREF] settings saved");
  }

  void setBrightnessValue(uint8_t value)
  {
    const uint8_t clamped = clampBrightness(value);
    if (brightness == clamped)
      return;
    brightness = clamped;
    scheduleSettingsSave();
    Serial.printf("[LAMP] brightness=%u\n", brightness);
    renderLamp();
  }

  void increaseBrightness() { setBrightnessValue(clampBrightness(brightness + kBrightnessStep)); }
  void decreaseBrightness() { setBrightnessValue(clampBrightness(static_cast<int>(brightness) - kBrightnessStep)); }

  void setVolumeValue(uint8_t value)
  {
    const uint8_t clamped = clampVolume(value);
    if (volume == clamped)
      return;
    volume = clamped;
    scheduleSettingsSave();
    if (aiState == AiState::SPEAKING)
      setVolume(volume);
    Serial.printf("[VOLUME] volume=%u\n", volume);
  }

  bool sendInterruptRequest()
  {
    JsonDocument doc;
    doc["type"] = "INTERRUPT_REQUEST";
    doc["timestamp"] = millis() / 1000;
    return sendJson(doc);
  }

  void processInterruptButton()
  {
#if LUMI_INTERRUPT_BUTTON_PIN >= 0
    const bool pressed = digitalRead(LUMI_INTERRUPT_BUTTON_PIN) == LOW;
    const uint32_t now = millis();
    if (pressed && !interruptButtonWasPressed && now - lastInterruptAt >= kInterruptDebounceMs)
    {
      lastInterruptAt = now;
      Serial.println("[INPUT] interrupt button pressed");
      sendInterruptRequest();
      if (aiState == AiState::SPEAKING || aiState == AiState::PROCESSING)
        setAiState(AiState::IDLE);
    }
    interruptButtonWasPressed = pressed;
#endif
  }

  // Tiến trình phụ (FreeRTOS Task) chạy độc lập trên Core 0 dùng để upload file ghi âm lên server qua HTTP POST

  bool sendAudioStart()
  {
    JsonDocument doc;
    doc["type"] = "AUDIO_START";
    JsonObject p = doc["payload"].to<JsonObject>();
    p["sample_rate"] = LUMI_AUDIO_SAMPLE_RATE;
    p["channels"] = 1;
    p["format"] = "PCM_16BIT";
    p["session_id"] = audioSessionId;
    doc["timestamp"] = millis() / 1000;
    return sendJson(doc);
  }

  bool flushAudioStreamChunk()
  {
    if (!audioStreamChunkSamples)
      return true;
    const size_t bytes = audioStreamChunkSamples * sizeof(audioStreamChunk[0]);
    if (!webSocketConnected || !webSocket.sendBIN(reinterpret_cast<uint8_t *>(audioStreamChunk), bytes))
    {
      Serial.println("[AUDIO] PCM stream write failed");
      return false;
    }
    audioStreamChunkSamples = 0;
    return true;
  }

  bool appendMicrophoneSamples(const int16_t *stereo, size_t frames)
  {
    const size_t maxSamples = static_cast<size_t>(LUMI_AUDIO_SAMPLE_RATE) * LUMI_MAX_RECORD_SECONDS;
    for (size_t i = 0; i < frames && recordedSamples < maxSamples; ++i)
    {
      audioStreamChunk[audioStreamChunkSamples++] = stereo[i * 2];
      ++recordedSamples;
      if (audioStreamChunkSamples == kAudioStreamChunkSamples && !flushAudioStreamChunk())
        return false;
    }
    return true;
  }

  void stopRecordingAndStream()
  {
    recording = false;
    if (!flushAudioStreamChunk())
    {
      setAiState(AiState::ERROR);
      return;
    }

    const size_t totalBytes = recordedSamples * sizeof(audioStreamChunk[0]);
    JsonDocument doc;
    doc["type"] = "AUDIO_END";
    JsonObject p = doc["payload"].to<JsonObject>();
    p["session_id"] = audioSessionId;
    p["duration_ms"] = static_cast<uint32_t>(recordedSamples * 1000UL / LUMI_AUDIO_SAMPLE_RATE);
    p["total_bytes"] = totalBytes;
    doc["timestamp"] = millis() / 1000;
    if (!sendJson(doc))
    {
      Serial.println("[AUDIO] AUDIO_END write failed");
      setAiState(AiState::ERROR);
      return;
    }

    Serial.println("[AUDIO] AUDIO_END sent, switching to PROCESSING");
    Serial.printf("[AUDIO] streamed %u PCM bytes\n", static_cast<unsigned>(totalBytes));
    recordedSamples = 0;
    audioSessionId[0] = '\0';
    setAiState(AiState::PROCESSING);
  }

  void startRecording(uint32_t now)
  {
    if (recording || playbackActive || !webSocketConnected)
      return;

    recordedSamples = 0;
    audioStreamChunkSamples = 0;
    snprintf(audioSessionId, sizeof(audioSessionId), "sess_%lu", static_cast<unsigned long>(now));
    if (!sendAudioStart())
    {
      Serial.println("[AUDIO] AUDIO_START write failed");
      audioSessionId[0] = '\0';
      setAiState(AiState::ERROR);
      return;
    }

    lastVoiceAt = now;
    recording = true;
    setAiState(AiState::RECORDING);
  }

  // Runs from loop(), never from the WebSocket callback. With I2S timeout set to
  // zero, this bounded write cannot stall WebSocket heartbeats or reconnect logic.
  void processPlayback()
  {
    if (!playbackActive || !i2sReady || !playbackUsedBytes)
      return;

    const size_t contiguousBytes = min(playbackUsedBytes, kPlaybackBufferBytes - playbackReadOffset);
    const size_t chunkBytes = min(contiguousBytes, kPlaybackWriteChunkBytes);
    const size_t written = i2s.write(playbackBuffer + playbackReadOffset, chunkBytes);
    if (!written)
      return;

    playbackReadOffset = (playbackReadOffset + written) % kPlaybackBufferBytes;
    playbackUsedBytes -= written;
    if (!playbackUsedBytes && playbackEndRequested)
    {
      Serial.println("[AUDIO] playback queue drained -> IDLE");
      setAiState(AiState::IDLE);
    }
  }

  // Hàm xử lý tín hiệu âm thanh thu được từ Micro thông qua I2S
  void processMicrophone()
  {
    if (!i2sReady || !microphoneEnabled || playbackActive)
      return; // Bỏ qua nếu I2S chưa sẵn sàng hoặc đang phát phản hồi

    int16_t stereo[kMicFramesPerTick * 2]; // Mảng tạm thời lưu dữ liệu âm thanh Stereo đọc từ Micro
    // Thực hiện đọc dữ liệu âm thanh thô từ giao thức I2S
    const size_t bytes = i2s.readBytes(reinterpret_cast<char *>(stereo), sizeof(stereo));
    const size_t frames = bytes / (sizeof(int16_t) * 2); // Quy đổi số byte đọc được sang số lượng khung mẫu
    if (!frames)
      return; // Trả về nếu không có dữ liệu nào được đọc

    uint32_t peak = 0; // Biến tìm mức đỉnh biên độ âm thanh trong lượt đọc này để phát hiện giọng nói
    for (size_t i = 0; i < frames; ++i)
    {
      const int32_t sample = stereo[i * 2];                     // Lấy mẫu từ kênh Trái (Left channel)
      const uint32_t magnitude = sample < 0 ? -sample : sample; // Tính giá trị biên độ tuyệt đối
      if (magnitude > peak)
        peak = magnitude; // Tìm biên độ lớn nhất (peak)
    }
    const uint32_t now = millis();
    // Cơ chế VAD đơn giản: Nếu biên độ âm thanh vượt quá ngưỡng lọc tiếng ồn được cấu hình
    if (peak > LUMI_VAD_THRESHOLD)
    {
      startRecording(now); // Tự động kích hoạt bắt đầu ghi âm nếu chưa ghi âm
      if (recording)
        lastVoiceAt = now; // Cập nhật lại mốc thời gian phát hiện tiếng nói cuối cùng
    }
    if (!recording)
      return; // Không làm gì tiếp theo nếu chưa ở trạng thái ghi âm

    // Down-mix left channel to mono PCM16 and send every completed 2 KB chunk.
    if (!appendMicrophoneSamples(stereo, frames))
    {
      recording = false;
      setAiState(AiState::ERROR);
      return;
    }

    const size_t maxSamples = static_cast<size_t>(LUMI_AUDIO_SAMPLE_RATE) * LUMI_MAX_RECORD_SECONDS;
    if (recordedSamples >= maxSamples || now - lastVoiceAt >= LUMI_VAD_SILENCE_MS)
      stopRecordingAndStream();
  }

  // Áp dụng lệnh điều khiển LED nhận về từ server
  void applyLedCommand(JsonObject p)
  {
    const char *action = p["action"] | ""; // Lấy tên hành động cần thực hiện
    if (!strcmp(action, "ON"))
    {
      lightPower = true; // Bật nguồn sáng của đèn
      lampColor = 0;     // Reset màu tự chọn về 0 (sử dụng bóng LED trắng mặc định)
      colorHex = "#FFF5E0";
    }
    else if (!strcmp(action, "OFF"))
      lightPower = false; // Tắt nguồn sáng của đèn
    else if (!strcmp(action, "SET_BRIGHTNESS"))
    {
      // Nhận và cài đặt độ sáng mới cho đèn
      const int requestedBrightness = p["brightness"] | static_cast<int>(brightness);
      setBrightnessValue(clampBrightness(requestedBrightness));
      lightPower = brightness > 0; // Tự động bật nguồn đèn nếu độ sáng > 0
    }
    else if (!strcmp(action, "SET_COLOR"))
    {
      // Nhận và cài đặt màu sắc mới cho đèn dạng chuỗi Hex
      const char *color = p["color"] | "#FFF5E0";
      lampColor = !strcmp(color, "#FFF5E0") ? 0 : parseHexColor(color);
      colorHex = color;
      lightPower = true; // Bật nguồn sáng đèn khi đổi màu
    }
    else
      return;
    setAiState(AiState::IDLE); // Return to the listening half-duplex mode.
    reportState();             // Gửi trạng thái mới cập nhật ngược lại cho server
  }

  // Xử lý các gói tin định dạng văn bản (JSON) nhận được từ server qua WebSocket
  void handleServerText(uint8_t *payload, size_t length)
  {
    if (!payload || !length || length > kMaxServerJsonBytes)
    {
      Serial.printf("[WS] Rejected JSON payload of %u bytes\n", static_cast<unsigned>(length));
      setAiState(AiState::ERROR);
      return;
    }
    JsonDocument doc;
    // Phân tích cú pháp chuỗi JSON
    if (deserializeJson(doc, payload, length))
    {
      Serial.printf("[WS] TEXT type=(invalid) payload=%.*s\n", static_cast<int>(length), reinterpret_cast<const char *>(payload));
      setAiState("ERROR"); // Gặp lỗi nếu định dạng JSON gửi từ server bị sai
      return;
    }
    const char *type = doc["type"] | ""; // Lấy loại gói tin
    Serial.printf("[WS] TEXT type=%s payload=%.*s\n", type, static_cast<int>(length), reinterpret_cast<const char *>(payload));
    JsonObject p = doc["payload"].as<JsonObject>();
    if (!strcmp(type, "LED_STATE"))
      // Nhận chỉ thị chuyển đổi trạng thái hiển thị của AI từ server
      setAiState(p["state"] | "IDLE");
    else if (!strcmp(type, "LED_COMMAND"))
      // Nhận lệnh điều khiển đèn trực tiếp từ người dùng thông qua server
      applyLedCommand(p);
    else if (!strcmp(type, "VOLUME_COMMAND"))
    {
      // Nhận lệnh thay đổi âm lượng loa
      const int requestedVolume = p["value"] | static_cast<int>(volume);
      setVolumeValue(clampVolume(requestedVolume));
      reportState();
    }
    else if (!strcmp(type, "AUDIO_RESPONSE_START"))
    {
      // The output peripheral is fixed to 16-bit stereo at 24 kHz.  Accept
      // mono PCM16 from the server by duplicating it into both I2S slots.
      const char *format = p["format"] | "PCM16";
      const uint32_t sampleRate = p["sample_rate"] | LUMI_TTS_SAMPLE_RATE;
      const uint8_t channels = p["channels"] | 1;
      Serial.printf("[AUDIO] AUDIO_RESPONSE_START: format=%s, sample_rate=%lu, channels=%u\n",
                    format, static_cast<unsigned long>(sampleRate), channels);
      if (strcmp(format, "PCM_16BIT"))
      {
        Serial.printf("[AUDIO] Unsupported playback format: %s\n", format);
        setAiState(AiState::ERROR);
        return;
      }
      if (channels != 1 && channels != 2)
      {
        Serial.printf("[AUDIO] Unsupported playback channel count: %u\n", channels);
        setAiState(AiState::ERROR);
        return;
      }
      if (sampleRate != LUMI_TTS_SAMPLE_RATE)
      {
        Serial.printf("[AUDIO] Server sample rate %lu is unsupported; expected %lu Hz\n",
                      static_cast<unsigned long>(sampleRate), static_cast<unsigned long>(LUMI_TTS_SAMPLE_RATE));
        setAiState(AiState::ERROR);
        return;
      }

      playbackInputChannels = channels;
      // Bắt đầu nhận âm thanh TTS phản hồi: Chuyển I2S sang tần số lấy mẫu 24kHz của tệp TTS
      Serial.println("[WS] audio response start -> SPEAKING");
      if (aiState == AiState::SPEAKING)
      {
        clearPlaybackBuffer();
        playbackEndRequested = false;
      }
      else
        setAiState(AiState::SPEAKING);
    }
    else if (!strcmp(type, "AUDIO_RESPONSE_END"))
    {
      // Kết thúc việc nhận âm thanh TTS phản hồi
      playbackEndRequested = true;
      if (!playbackUsedBytes)
      {
        Serial.println("[WS] audio response end -> IDLE");
        setAiState(AiState::IDLE);
      }
    }
    else if (!strcmp(type, "ERROR"))
      setAiState("ERROR"); // Gặp lỗi
  }

  // Hàm xử lý sự kiện kết nối WebSocket
  void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
  {
    if (type == WStype_CONNECTED)
    {
      webSocketConnected = true; // Đã kết nối thành công tới WebSocket Server
      Serial.println("[WS] connected -> IDLE");
      setAiState(AiState::IDLE);
      sendHeartbeat(); // Gửi bản tin Heartbeat ngay khi kết nối
      reportState();   // Báo cáo trạng thái hiện tại lên server
    }
    else if (type == WStype_DISCONNECTED)
    {
      webSocketConnected = false; // Mất kết nối WebSocket
      Serial.println("[WS] disconnected -> ERROR");
      setAiState(AiState::ERROR);
    }
    else if (type == WStype_TEXT)
      handleServerText(payload, length); // Nhận dữ liệu cấu hình/văn bản
    else if (type == WStype_BIN)
    {
      Serial.printf("[AUDIO] BIN bytes=%u\n", static_cast<unsigned>(length));
      // Some servers omit AUDIO_RESPONSE_START; the first binary frame is safe
      // to use as the speaking transition point.
      if (aiState != AiState::SPEAKING)
      {
        Serial.println("[WS] binary audio received -> SPEAKING");
        setAiState(AiState::SPEAKING);
      }
      bool queued = false;
      if (playbackInputChannels == 1)
        queued = queueMonoPlaybackAudio(payload, length);
      else
      {
        if (length % (sizeof(int16_t) * 2))
          Serial.printf("[AUDIO] Rejected stereo PCM frame with incomplete sample: %u bytes\n", static_cast<unsigned>(length));
        else
          queued = queuePlaybackAudio(payload, length);
      }
      if (!queued)
        Serial.println("[WS] BIN audio frame rejected");
    }
  }

  // Tự động kiểm tra trạng thái và thực hiện kết nối lại WiFi khi bị rớt mạng
  void reconnectWiFi()
  {
    if (!hasNetworkConfiguration())
      return;
    if (WiFi.status() == WL_CONNECTED || millis() - lastWifiAttemptAt < kWifiRetryMs)
      return; // Bỏ qua nếu vẫn đang kết nối hoặc chưa hết thời gian chờ thử lại
    lastWifiAttemptAt = millis();
    if (aiState != AiState::BOOTING && aiState != AiState::WIFI_CONNECTING)
      setAiState(AiState::WIFI_CONNECTING);
    WiFi.disconnect();                              // Ngắt kết nối cũ
    WiFi.begin(LUMI_WIFI_SSID, LUMI_WIFI_PASSWORD); // Bắt đầu kết nối lại WiFi với SSID và mật khẩu cấu hình
  }
} // namespace

// Hàm cài đặt ban đầu (chạy một lần duy nhất khi khởi động thiết bị)
void setup()
{
  Serial.begin(115200); // Khởi tạo cổng truyền thông nối tiếp để debug ở tốc độ Baud 115200
  if (preferences.begin("lumi-ai", false))
  {
    brightness = clampBrightness(preferences.getUChar("brightness", kBrightnessDefault));
    if (preferences.getUChar("volume_profile_version", 0) != kVolumeProfileVersion)
    {
      volume = kVolumeDefault;
      preferences.putUChar("volume", volume);
      preferences.putUChar("volume_profile_version", kVolumeProfileVersion);
      Serial.printf("[PREF] migrated volume default to %u\n", volume);
    }
    else
      volume = clampVolume(preferences.getUChar("volume", kVolumeDefault));
    preferences.end();
  }
  else
    Serial.println("[PREF] Failed to open NVS for reading");
  Serial.printf("[PREF] brightness=%u volume=%u\n", brightness, volume);
  if (!hasNetworkConfiguration())
    Serial.println("[CONFIG] Missing config.local.h credentials or server endpoint");
  WiFi.mode(WIFI_STA);         // Thiết lập chế độ kết nối WiFi là Station (kết nối vào Access Point có sẵn)
  WiFi.setAutoReconnect(true); // Cấu hình tự động kết nối lại WiFi ở mức hệ thống
  WiFi.persistent(false);      // Không lưu thông tin WiFi vào bộ nhớ Flash để kéo dài tuổi thọ chip
  if (LUMI_INTERRUPT_BUTTON_PIN >= 0)
    pinMode(LUMI_INTERRUPT_BUTTON_PIN, INPUT_PULLUP);
  reconnectWiFi(); // Bắt đầu kết nối WiFi

  // Khởi tạo dải đèn LED nếu chân GPIO cấu hình hợp lệ
  if (LUMI_LED_DATA_PIN >= 0)
  {
    strip.begin(); // Khởi động thư viện điều khiển NeoPixel
    strip.clear(); // Xóa sạch dữ liệu màu cũ
    strip.show();  // Cập nhật trạng thái tắt toàn bộ LED ban đầu
    ledReady = true;
    renderLamp(); // Cập nhật màu ban đầu theo trạng thái
  }

  // Khởi tạo bus giao tiếp I2C cho chip âm thanh ES8311 (SDA, SCL)
  Wire.begin(LUMI_ES8311_SDA, LUMI_ES8311_SCL);
  pinMode(LUMI_ES8311_PA_ENABLE, OUTPUT);   // Thiết lập chân điều khiển bật/tắt IC khuếch đại âm thanh (PA) là đầu ra
  digitalWrite(LUMI_ES8311_PA_ENABLE, LOW); // Start safely muted until SPEAKING.
  setAiState(AiState::WIFI_CONNECTING);
  codecReady = codec.begin(Wire); // Cấu hình ban đầu cho chip giải mã ES8311
  if (!codecReady)
  {
    setAiState("ERROR");
    Serial.println("ES8311 not detected at 0x18 or 0x19");
  }
  // Khởi động giao tiếp I2S để đọc âm thanh ghi âm mặc định ở tần số 16kHz
  if (!configureI2S(LUMI_AUDIO_SAMPLE_RATE))
  {
    setAiState("ERROR");
    Serial.println("I2S initialization failed");
  }
  else if (aiState != AiState::ERROR)
  {
    // IDLE is a listening-capable state: mic ON, speaker physically muted.
    switchToListeningMode();
  }

  // Khởi động kết nối WebSocket Client tới máy chủ dựa trên mã định danh thiết bị
  if (hasNetworkConfiguration())
  {
    char path[96];
    const int pathLength = snprintf(path, sizeof(path), "/ws/device/%s", LUMI_DEVICE_CODE);
    if (pathLength > 0 && static_cast<size_t>(pathLength) < sizeof(path))
    {
#if LUMI_USE_TLS
      webSocket.beginSslWithCA(LUMI_SERVER_HOST, LUMI_SERVER_PORT, path, LUMI_TLS_ROOT_CA, "arduino");
#else
      webSocket.begin(LUMI_SERVER_HOST, LUMI_SERVER_PORT, path);
#endif
      webSocket.onEvent(webSocketEvent);
      webSocket.setReconnectInterval(5000);
      setAiState(AiState::WS_CONNECTING);
    }
    else
    {
      Serial.println("[CONFIG] WebSocket path is too long");
      setAiState(AiState::ERROR);
    }
  }
  else
    setAiState(AiState::ERROR);
}

// Vòng lặp chính xử lý tác vụ (chạy lặp đi lặp lại liên tục)
void loop()
{
  reconnectWiFi(); // Kiểm tra và kết nối lại WiFi khi cần thiết
  processInterruptButton();
  // Service I2S before network/JSON work so a busy WebSocket iteration cannot
  // starve an already-buffered PCM stream.
  processPlayback();
  if (WiFi.status() == WL_CONNECTED)
    webSocket.loop(); // Duy trì các tiến trình trao đổi dữ liệu ngầm của WebSocket
  // Gửi bản tin Heartbeat định kỳ lên server để giữ phiên kết nối
  if (webSocketConnected && millis() - lastHeartbeatAt >= kHeartbeatMs)
    sendHeartbeat();
  processPlayback();
  // Half-duplex gate: never read microphone samples while processing, speaking,
  // or in an error state.
  if (aiState == AiState::IDLE || aiState == AiState::LISTENING || aiState == AiState::RECORDING)
    processMicrophone();
  if (aiState == AiState::ERROR && codecReady && hasNetworkConfiguration() && millis() - lastErrorAt >= kErrorRecoveryMs)
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("[AI_STATE] retrying Wi-Fi after error");
      setAiState(AiState::WIFI_CONNECTING);
    }
    else if (!webSocketConnected)
    {
      Serial.println("[AI_STATE] waiting for WebSocket reconnect after error");
      setAiState(AiState::WS_CONNECTING);
    }
    else
    {
      Serial.println("[AI_STATE] retrying listening mode after error");
      setAiState(AiState::IDLE);
    }
  }
  if (aiState == AiState::PROCESSING && millis() - lastProcessingAt >= kProcessingTimeoutMs)
  {
    Serial.println("[AI_STATE] PROCESSING timeout — server did not respond, returning to IDLE");
    setAiState(AiState::IDLE);
  }
  flushSettingsIfDue();
  delay(1);
}
