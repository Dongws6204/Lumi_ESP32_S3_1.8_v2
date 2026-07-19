# Kế hoạch thực thi đã xác minh — LumiAIver2 / xiaozhi-esp32

> Tài liệu handoff cho coding agent. Mọi kết luận bên dưới đã được đối chiếu với source và cấu hình hiện có ngày 2026-07-15. Nếu code thay đổi trước khi thực hiện, agent phải đọc lại các file được nêu ở từng task; source hiện tại là nguồn quyết định cuối cùng.

## 1. Baseline bắt buộc

- **Project root:** `C:\LumiAIver2\xiaozhi-esp32`
- **Build:** ESP-IDF v5.5.4, target `esp32s3`; workspace đã được cấu hình. Lệnh build chuẩn: `idf.py build`.
- **Board thực tế:** `waveshare/esp32-s3-touch-amoled-1.8-v2` (`sdkconfig` bật `CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_8_v2=y`). Codec là ES8311, input/output 24 kHz, mono input; `input_reference_ = false`, nghĩa là không có đường I2S loopback reference cho AEC phần cứng.
- **Voice config:** `CONFIG_USE_AFE_WAKE_WORD=y`, `CONFIG_SEND_WAKE_WORD_DATA=y`, `CONFIG_USE_AUDIO_PROCESSOR=y`, ngôn ngữ `vi-VN`.
- Không sửa `sdkconfig`, board pinout, OTA endpoint/activation, protocol MQTT/UDP/Opus/AES, LED, touch hay màn hình nếu task không yêu cầu rõ.
- Workspace gốc đang có thay đổi/xóa ngoài phạm vi `PLAN.md`. Không reset, checkout, clean hoặc khôi phục file không liên quan.

## 2. Quy tắc nghiệm thu cho từng task

1. Đọc toàn bộ các file “Đọc trước” của task trước khi sửa; không suy đoán từ kế hoạch cũ.
2. Mỗi task là một thay đổi độc lập, dễ review. Không gộp task khác vào cùng diff.
3. Chạy `idf.py build` thành công sau khi sửa. Nếu môi trường thiếu ESP-IDF hoặc build lỗi không liên quan, ghi nguyên văn command, lỗi và lý do; không tuyên bố đã pass build.
4. Khi bàn giao mỗi task, đưa: `git diff` thật của task, command build và kết quả, log UART/thiết bị của scenario (hoặc nêu rõ không thể flash), cùng các rủi ro còn lại.
5. Không coi “build thành công” là chứng minh hành vi audio/timer. Các test phần cứng bên dưới vẫn bắt buộc trước khi đóng task.

## 3. Bản đồ luồng đã xác minh

| Luồng | Sự thật hiện tại | Hệ quả cho kế hoạch |
|---|---|---|
| Wake word → kết nối | `HandleWakeWordDetectedEvent()` đặt state `Connecting`, rồi `ContinueWakeWordInvoke()` gọi `OpenAudioChannel()`. | Failure hiện để state kẹt `Connecting`. Đây là lỗi P0 thực sự. |
| Nút/start listening → kết nối | `HandleStartListeningEvent()` dùng `ContinueOpenAudioChannel()`, cũng gọi `OpenAudioChannel()`. | Có cùng lỗi kẹt state, phải sửa trong cùng task P0-1. |
| Phát nhạc | `MusicPlayer::Play()` chủ động `EnableVoiceProcessing(false)`, `ResetDecoder()`, sau đó chỉ bật wake word AFE. | Không được “re-arm” full STT ngay sau `Play()`; điều đó phá thiết kế chống tự nghe loa. |
| Nói khi nhạc phát | `HandleWakeWordDetectedEvent()` thấy nhạc active thì `StopMusic()` rồi chuyển `IdleSleep`; wake word tiếp theo/luồng hiện tại mở channel và vào listening. | Test đúng là nói **“LuMi, dừng nhạc”** (hoặc wake word rồi câu lệnh), không kỳ vọng STT liên tục khi nhạc đang phát. |
| Timer JSON-RPC | `CloseAudioChannel()` của MQTT chỉ reset UDP/audio session; nó không hủy `protocol_`. `SendMcpMessage()` được schedule lên main task và `MqttProtocol::SendText()` trả `false` khi publish lỗi. | Không có bằng chứng null-deref/crash P1-1 như kế hoạch cũ nêu. Không thêm guard võ đoán. |

## P0 — sửa ngay

### P0-1. Thoát khỏi `Connecting` khi `OpenAudioChannel()` thất bại

**Trạng thái:** [Đã hoàn thành] Đã fix lỗi kẹt `Connecting` bằng fallback về `IdleSleep`.

**Đọc trước:**

- `main/application.cc`
- `main/application.h`
- `main/protocols/protocol.h`
- `main/protocols/mqtt_protocol.cc` (ít nhất `OpenAudioChannel()` và `CloseAudioChannel()`)

**Bằng chứng source:**

- `ContinueWakeWordInvoke()` gọi `OpenAudioChannel()` và khi `false` chỉ bật wake word rồi `return`.
- `ContinueOpenAudioChannel()` cũng `return` trực tiếp khi `OpenAudioChannel()` thất bại.
- Cả hai đều đã đặt `kDeviceStateConnecting`; event wake word sau đó không thuộc nhánh `IsIdleLike`, `Speaking`, `Listening` hay `Activating`, nên request mới bị bỏ qua.

**Thay đổi yêu cầu:**

1. Tạo một đường recovery dùng chung, hoặc thực hiện nhất quán ở cả hai hàm trên. Sau failure phải:
   - log rõ context (`wake_word`/`start_listening`) và failure;
   - bật lại wake word khi cần để thiết bị nhận lệnh tiếp theo;
   - đưa state về `kDeviceStateIdleSleep` qua `EnterIdleSleep("...")` hoặc cơ chế tương đương đã kiểm chứng.
2. Không gọi `SetDeviceState(kDeviceStateIdleSleep)` trực tiếp nếu bỏ qua cleanup audio/power mà `EnterIdleSleep()` đang đảm nhiệm.
3. Giữ nguyên nhánh thành công, bao gồm `SendWakeWordDetected()` và `SetListeningMode()`.
4. Rà lại null safety: hai continuation chỉ chạy khi `protocol_` còn hợp lệ. Nếu thêm guard cho `protocol_ == nullptr`, recovery phải vẫn đưa UI/state khỏi `Connecting`.

**Không làm:** retry vòng lặp/blocking trong main task, thay đổi timeout của MQTT, hoặc mở audio channel từ ISR.

**Test thiết bị bắt buộc:**

1. Boot đến `IdleSleep` và xác nhận wake word hoạt động khi mạng ổn định.
2. Ép `OpenAudioChannel()` fail (tắt AP/router hoặc chặn Internet) đúng khi gọi wake word. Lặp lại riêng bằng BOOT/start-listening nếu board UI hỗ trợ.
3. Xác nhận log có failure context và transition về `idle_sleep`; không còn state `connecting` dai dẳng.
4. Khôi phục mạng, gọi wake word lần nữa. Thiết bị phải mở channel, vào listening và nhận STT bình thường không reboot.

**Tiêu chí pass:** build pass; cả hai entry point recover; lần tương tác sau failure hoạt động.

---

### P0-2. Xác minh và khôi phục điều khiển bằng giọng nói trong lúc phát nhạc — không bật STT song song một cách mù quáng

**Trạng thái:** Hành vi nền đã xác minh; chỉ sửa khi test tái hiện lỗi với câu đánh thức đúng.

**Đọc trước:**

- `main/application.cc`: `PlayMusic`, `StartPendingMusicAfterTts`, `HandleWakeWordDetectedEvent`, `HandleStateChangedEvent`
- `main/audio/music_player.cc`
- `main/audio/audio_service.cc` và `.h`
- `main/audio/wake_words/afe_wake_word.cc`
- `sdkconfig`

**Sự thật source cần giữ:**

```cpp
// main/audio/music_player.cc, MusicPlayer::Play()
audio_service.EnableVoiceProcessing(false);
audio_service.ResetDecoder();
audio_service.EnableWakeWordDetection(audio_service.IsAfeWakeWord());
```

Board hiện bật AFE wake word, do đó wake word vẫn là đường điều khiển trong khi nhạc phát. Full audio processor/STT chỉ được bật lại khi state sang listening (`HandleStateChangedEvent`/`HandleStartListeningEvent`). `StartPendingMusicAfterTts()` gọi thẳng `MusicPlayer::Play()`, nên đã nhận đúng cùng chính sách audio; không cần nhân bản lệnh ở call site.

**Test trước khi quyết định code change:**

1. Phát một URL HTTPS/Ogg-Opus hợp lệ tới khi log có `state=PLAYING`.
2. Không chạm màn hình. Nói đủ wake word + lệnh, ví dụ “LuMi, dừng nhạc”.
3. Log kỳ vọng: wake word detected → `[MUSIC] wake-word interrupt: stop music` → audio channel/listening → `Application: >> ...`. Nhạc phải dừng và thiết bị không treo.
4. Lặp lại với nhạc pending: tạo TTS trước, để log có `[MUSIC] TTS finished, starting pending music`, rồi thực hiện bước 2.

**Nếu test pass:** không sửa code; ghi kết quả là hành vi mong đợi. Quan sát “16 giây không có `>>`” không đủ kết luận vì STT không được thiết kế chạy liên tục khi music play.

**Nếu test fail:** chỉ khi log chứng minh AFE wake word không chạy/không phát event trong lúc nhạc phát, mới sửa theo nguyên nhân cụ thể. Trước tiên log `IsWakeWordRunning()`, `IsAfeWakeWord()` và state; kiểm tra callback wake word. Không bật `EnableVoiceProcessing(true)` song song với nhạc trừ khi có yêu cầu sản phẩm mới, thiết kế AEC/CPU riêng và test hồi quy đầy đủ.

**Tiêu chí pass:** build pass; 3 lần liên tiếp câu wake word + stop music thành công khi nhạc đang phát, không cần BOOT/touch.

## P1 — độ tin cậy và hành vi

### P1-1. Reschedule timer cùng action, không chồng timer

**Trạng thái:** [Đã hoàn thành] Đã thêm dedupe/lifecycle mutex cho timer context.

**Đọc trước:**

- `main/application.cc` (`DeviceTimerContext`, `SetDeviceTimer`, callback, `ExecuteDeviceTimerAction`)
- `main/application.h`
- ESP-IDF `esp_timer` API sẵn trong môi trường đang build (để tuân thủ vòng đời stop/delete/callback)

**Bằng chứng source:** Mỗi `SetDeviceTimer()` `new DeviceTimerContext`, `esp_timer_create`, `esp_timer_start_once`; context chỉ bị xóa khi timer bắn hoặc tạo task thất bại. Không có registry hay cancel theo action.

**Quyết định sản phẩm để implement:** action key là **chuỗi `action` chính xác**. Hai chuỗi JSON khác byte (khác thứ tự field/khoảng trắng) là hai action khác; không tự viết canonical JSON trong task này. Lý do: API hiện truyền một chuỗi opaque và chưa có định nghĩa “cùng action” theo ngữ nghĩa.

**Thay đổi yêu cầu:**

1. Lưu timer pending theo action ở `Application`, có mutex bảo vệ vì callback timer và main task khác ngữ cảnh.
2. Khi đặt lại cùng action, dừng/hủy timer cũ an toàn trước khi đăng ký timer mới; log phải phân biệt `scheduled`, `replaced`, `triggered`, `cancelled`.
3. Thiết kế ownership để không có use-after-free/double-delete khi cancel trùng lúc callback hoặc task action đang chạy. Không giữ raw pointer bị xóa từ cả callback và main task.
4. Khi timer bắn hoặc create/start fail, xóa đúng entry registry. Không ảnh hưởng timer action khác.
5. Không đổi schema `set_device_timer` và không làm callback esp_timer chạy MCP/logic nặng trực tiếp.

**Test thiết bị:**

1. Đặt action `stop_music` sau 1 phút, sau đó ngay lập tức đặt lại cùng chuỗi sau 2 phút.
2. Duy trì nhạc cho tới hết phút thứ nhất: nhạc **không** được dừng; tới phút thứ hai nhạc dừng đúng một lần. Log có `replaced` và một `triggered`.
3. Đặt hai action khác nhau (ví dụ `stop_music` và `idle_sleep`), xác minh cả hai còn độc lập.
4. Thử reschedule sát thời điểm bắn nhiều lần; không crash, không double free, không action lặp.

**Tiêu chí pass:** build pass, test 1–4 pass, heap không giảm bất thường sau lặp ít nhất 10 lượt đặt lại.

---

### P1-2. Timer JSON-RPC khi `IdleSleep` — chỉ regression test, không phải crash fix

**Trạng thái:** Giả thuyết crash trong bản kế hoạch cũ bị bác bỏ bởi source hiện tại.

**Bằng chứng:** `MqttProtocol::CloseAudioChannel()` chỉ `udp_.reset()`, còn đối tượng `protocol_` và MQTT control channel còn tồn tại. `Application::SendMcpMessage()` kiểm tra `protocol_` trong callback scheduled; `MqttProtocol::SendText()` trả lỗi khi không publish được thay vì dereference UDP.

**Việc thực hiện:**

1. Không thêm guard chặn `McpServer::ParseMessage()` chỉ vì `IdleSleep`.
2. Chạy regression: đặt timer 1 phút với JSON-RPC `tools/call` cho một tool local đã đăng ký, để thiết bị về `IdleSleep`; xác nhận action chạy, không reboot/panic và log phản hồi rõ (publish thành công hoặc failed-to-publish nếu mất mạng).
3. Nếu phát hiện crash thật, lưu backtrace/log và tạo task mới theo stack trace. Không biến test fail thành suy đoán null pointer.

**Rủi ro cần ghi nhận:** JSON-RPC tool call có thể tạo response về control channel dù timer là local. Nếu sản phẩm yêu cầu timer hoàn toàn offline/no-response, đó là thay đổi API riêng, cần owner xác nhận trước khi sửa.

---

### P1-3. Cải thiện nhận lệnh ngủ tiếng Việt với phạm vi hẹp

**Trạng thái:** [Đã hoàn thành] Đã thêm các câu lệnh tiếng Việt UTF-8 phổ biến.

**Đọc trước:** `main/application.cc`, đặc biệt `ToLowerAscii()` và `ContainsSleepCommand()`.

**Bằng chứng:** `ToLowerAscii()` gọi `tolower` từng byte, không case-fold UTF-8. `ContainsSleepCommand()` chỉ khớp ASCII không dấu và hai biến thể có dấu viết hoa đầu câu.

**Thay đổi yêu cầu:**

1. Trước khi hard-code, chốt danh sách câu được phép với chủ sản phẩm. Danh sách tối thiểu đề xuất: `tắt đi`, `đi ngủ`, `tắt máy`, `ngủ thôi`, `nghỉ đi`, cộng biến thể ASCII đã có.
2. Với scope nhỏ này, dùng bảng phrase UTF-8 có chủ đích (bao gồm lower-case và dạng STT thường viết hoa đầu câu) sau `ToLowerAscii()`; đây là mở rộng coverage, **không** được gọi là Unicode normalization tổng quát.
3. Không thêm ICU/thư viện lớn chỉ để so khớp vài cụm. Nếu cần case-fold tiếng Việt tổng quát, tách task thiết kế dependency/footprint riêng.
4. Giữ match theo cụm (substring) để các câu dài tự nhiên hoạt động; tránh keyword quá ngắn gây false positive.

**Test:** Đưa trực tiếp/qua STT ít nhất các câu: “thôi tắt đi nha”, “tắt đi”, “đi ngủ nhé”, “ngủ thôi”, “nghỉ đi”, “tắt máy”. Mỗi câu phải dẫn tới `EnterIdleSleep("voice_sleep_command")`; một câu phủ định như “đừng tắt máy” phải được kiểm tra thủ công và ghi rõ hành vi quyết định.

**Tiêu chí pass:** build pass, danh sách đã được owner chốt, các positive pass và không có false-positive nghiêm trọng đã biết.

## P2 — chất lượng mic, chỉ sau P0-2

### P2-1. Đo và cải thiện phạm vi wake word

**Trạng thái:** Cần đo trên hardware, không đủ bằng chứng để đổi gain/sample rate.

**Sự thật baseline:** ES8311 khởi tạo gain input `34`; board dùng 24 kHz. `LogAecLoopbackReference()` sẽ báo `inactive` vì `Es8311AudioCodec::input_reference_ = false`; đây là giới hạn phần cứng/driver, không phải lỗi log. Không suy luận rằng đổi sample rate 24 kHz → 16 kHz sẽ tự cải thiện SNR.

**Trình tự thực hiện:**

1. Ghi baseline: phòng, mức ồn, âm lượng loa, vị trí thiết bị, khoảng cách; chạy 10 lần wake word ở mỗi mốc khoảng cách. Mục tiêu định lượng: tỉ lệ thành công ≥80%/10 lần.
2. Kiểm tra vật lý trước: lỗ mic, chiều mic/case, nguồn cấp, vị trí loa và clipping. Lưu log `[AEC]` để xác nhận giới hạn loopback đã biết.
3. Xác định gain runtime bằng log/source; chỉ thử thay đổi từng nấc nhỏ và có thể đảo ngược. Mỗi nấc phải chạy lại toàn bộ baseline và P0-2 (wake word ngắt nhạc).
4. Chỉ khảo sát 16 kHz sau khi có số đo chứng minh resampler là vấn đề; kiểm tra codec/AFE compatibility và toàn bộ regression audio trước khi giữ thay đổi.

**Tiêu chí pass:** số liệu trước/sau được ghi rõ; không giảm tỉ lệ wake word, không gây clipping/false wake không chấp nhận được, P0-2 vẫn pass.

## 4. Thứ tự thực hiện

1. P0-1 — sửa cả hai đường mở audio channel và test recovery.
2. P0-2 — chạy test phân loại hành vi; chỉ mở task code nếu AFE wake word thật sự fail.
3. P1-1 — lifecycle timer/dedupe.
4. P1-2 — regression timer JSON-RPC khi idle.
5. P1-3 — sau khi có danh sách phrase được chủ sản phẩm chốt.
6. P2-1 — chỉ sau P0-2 pass ổn định.

## 5. Không làm theo kế hoạch cũ

- Không còn mục “cần thêm `music_player.cc`/`protocol.cc`/board file”: cả ba đã có và đã được kiểm tra trong workspace này.
- Không thêm `EnableVoiceProcessing(true)` sau `MusicPlayer::Play()` hoặc `StartPendingMusicAfterTts()`. Đây sẽ đi ngược kiến trúc hiện tại và có nguy cơ tự nghe loa.
- Không coi `CloseAudioChannel()` là hủy `protocol_` trong MQTT; source chứng minh ngược lại.
- Không đổi gain, sample rate hay AEC chỉ dựa trên cảm nhận hoặc một log STT vắng mặt.

## 6. Phân tích các luồng xử lý âm thanh hiện tại

Dựa trên việc kiểm tra mã nguồn (`application.cc` và các module audio), thiết kế luồng xử lý âm thanh hiện tại của hệ thống được chia thành các luồng chính sau:

1. **Luồng chờ đánh thức (Wake-up Flow / Idle)**:
   - **Trạng thái**: `IdleSleep`.
   - **Hành động**: Thiết bị ở chế độ tiết kiệm năng lượng. Hệ thống tắt nhận dạng giọng nói toàn diện (`EnableVoiceProcessing(false)`), chỉ duy trì bộ nhận diện từ khóa đánh thức (AFE Wake Word).
   - **Chuyển trạng thái**: Khi nhận diện từ khóa (ví dụ "LuMi"), sự kiện `HandleWakeWordDetectedEvent` được kích hoạt. Thiết bị chuyển sang `Connecting`, gọi `OpenAudioChannel()`. Nếu kết nối thành công, hệ thống vào trạng thái `Listening`, bật mic và truyền âm thanh lên server (`EnableVoiceProcessing(true)`). Nếu lỗi kết nối, hệ thống fallback an toàn về lại `IdleSleep` (fix P0-1).

2. **Luồng ra lệnh và phản hồi AI (Voice Command & TTS Flow)**:
   - **Trạng thái**: `Listening` → `Speaking`.
   - **Hành động**: Sau khi nhận câu lệnh, server trả về phản hồi dưới dạng âm thanh (TTS). Khi bắt đầu phát TTS, thiết bị chuyển trạng thái thành `Speaking`.
   - **Bảo vệ VAD/AEC**: Để AI không tự phản hồi lại giọng nói của chính mình phát ra từ loa, `EnableVoiceProcessing(false)` được gọi để tắt streaming lên server tạm thời. Sau khi phát xong TTS, thiết bị tự động về lại `Listening` (nếu `listening_mode_` là always-on) hoặc `IdleSleep`.

3. **Luồng phát nhạc (Music Playing Flow)**:
   - **Trạng thái**: `Playing`.
   - **Hành động**: Khi có lệnh phát nhạc, `MusicPlayer::Play()` được gọi. Hàm này chủ động tắt Full Voice Processing và reset decoder, chỉ giữ lại AFE Wake Word để chờ lệnh ngắt. Thiết kế này ngăn việc nghe nhầm âm thanh nhạc thành lệnh do hardware không có I2S loopback hoàn hảo (`input_reference_ = false`).

4. **Luồng ngắt/dừng nhạc (Music Interruption / Stop Command)**:
   - **Trạng thái**: Đang `Playing`.
   - **Hành động**: Dù nhạc đang phát, AFE Wake Word vẫn hoạt động ngầm. Khi người dùng nói từ khóa đánh thức, `HandleWakeWordDetectedEvent` phát hiện có nhạc đang phát và sẽ ưu tiên gọi `StopMusic()`. Nhạc bị dừng lập tức, thiết bị về `IdleSleep` thoảng qua, rồi mở kết nối (Connecting) để bắt đầu nghe (Listening) câu lệnh tiếp theo (ví dụ: "dừng nhạc").
   - **Lệnh tắt cục bộ**: Nếu đang ở trạng thái `Listening` mà người dùng nói "tắt đi", "đi ngủ", "tắt máy"... hàm `ContainsSleepCommand()` sẽ nhận diện bằng local match (UTF-8/ASCII) và gọi ngay `EnterIdleSleep()`, tắt các kết nối audio nhanh chóng mà không cần chờ LLM.
