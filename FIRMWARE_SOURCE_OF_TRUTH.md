# Firmware Source Of Truth

Repo: `C:\LumiAIver2\xiaozhi-esp32`

This handoff is based on the current workspace source/config plus existing local logs where noted. If an older report conflicts with source, source wins.

## 1. Build environment

- ESP-IDF: v5.5.4 from `sdkconfig:3`.
- Target: `esp32s3` from `sdkconfig:391`.
- Selected board config: `CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_8_v2=y` from `sdkconfig:683`.
- Board CMake mapping: `main/CMakeLists.txt:324-326` maps that config to manufacturer `waveshare` and board type `esp32-s3-touch-amoled-1.8-v2`.
- Board config JSON: `main/boards/waveshare/esp32-s3-touch-amoled-1.8-v2/config.json:2-8` confirms target `esp32s3` and build name `esp32-s3-touch-amoled-1.8-v2`.
- Partition table currently selected: `partitions/v2/16m.csv` from `sdkconfig:593`.
- Correct build command for this already-configured workspace:

```powershell
idf.py build
```

- Do not run `install.ps1`. No build, flash, erase, or toolchain install was performed for this document.

## 2. Board hardware

Board: Waveshare ESP32-S3 Touch AMOLED 1.8 V2.

Primary board files:

- `main/boards/waveshare/esp32-s3-touch-amoled-1.8-v2/config.h`
- `main/boards/waveshare/esp32-s3-touch-amoled-1.8-v2/esp32-s3-touch-amoled-1.8-v2.cc`
- Class: `WaveshareEsp32s3TouchAMOLED1inch8`, declared around `esp32-s3-touch-amoled-1.8-v2.cc:405`.
- Board registration: `DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED1inch8)` around `esp32-s3-touch-amoled-1.8-v2.cc:922`.

Audio codec:

- ES8311 is included at `esp32-s3-touch-amoled-1.8-v2.cc:8`.
- `GetAudioCodec()` constructs `Es8311AudioCodec` around `esp32-s3-touch-amoled-1.8-v2.cc:867-872`.
- `Es8311AudioCodec` works in `ESP_CODEC_DEV_WORK_MODE_BOTH` around `main/audio/codecs/es8311_audio_codec.cc:43-52`.
- Input/output sample rate for this board: `24000` from `config.h:6-7`.
- I2S/I2C/PA pins from `config.h:9-18`:
  - MCLK `GPIO16`
  - WS/LRCK `GPIO45`
  - BCLK `GPIO9`
  - DIN `GPIO10`
  - DOUT `GPIO8`
  - PA `GPIO46`
  - ES8311 I2C SDA `GPIO15`
  - ES8311 I2C SCL `GPIO14`
  - ES8311 addr `ES8311_CODEC_DEFAULT_ADDR`
- I2S duplex channel config is in `main/audio/codecs/es8311_audio_codec.cc:100-149`.

Display/touch/button:

- AMOLED panel driver include: `esp_lcd_co5300.h` at `esp32-s3-touch-amoled-1.8-v2.cc:2`.
- Display size: `368x448` from `config.h:30-31`; offsets `x=16`, `y=0` from `config.h:36-37`.
- QSPI display pins from `config.h:23-29`:
  - CS `GPIO12`
  - PCLK `GPIO11`
  - DATA0 `GPIO4`
  - DATA1 `GPIO5`
  - DATA2 `GPIO6`
  - DATA3 `GPIO7`
  - RST `GPIO_NUM_NC`
- `InitializeSpi()` uses `SPI2_HOST` and the same QSPI pins around `esp32-s3-touch-amoled-1.8-v2.cc:632-642`.
- `InitializeDisplay()` creates the CO5300 panel and `CustomLcdDisplay` around `esp32-s3-touch-amoled-1.8-v2.cc:736-771`.
- Backlight is command-based through `CustomBacklight::SetBrightnessImpl()` around `esp32-s3-touch-amoled-1.8-v2.cc:386-402`; `DISPLAY_BACKLIGHT_PIN` is `GPIO_NUM_NC` in `config.h:39`.
- Touch controller: CST816S from `esp_lcd_touch_cst816s.h` at `esp32-s3-touch-amoled-1.8-v2.cc:28`.
- Touch reset/interrupt pins: reset `GPIO39`, interrupt `GPIO13` around `esp32-s3-touch-amoled-1.8-v2.cc:773-817`.
- Boot button: `BOOT_BUTTON_GPIO GPIO_NUM_0` from `config.h:21`; click handler around `esp32-s3-touch-amoled-1.8-v2.cc:644-665`.

SK6812 RGBWW:

- Data pin: `LUMI_LED_DATA_PIN GPIO_NUM_42` from `config.h:42`.
- LED count: `LUMI_LED_COUNT 30` from `config.h:43`.
- Class: `Sk6812RgbwwStrip` around `esp32-s3-touch-amoled-1.8-v2.cc:141`.
- Strip config uses `LED_STRIP_COLOR_COMPONENT_FMT_GRBW` and `LED_MODEL_SK6812` around `esp32-s3-touch-amoled-1.8-v2.cc:143-149`.
- Default brightness: `DEFAULT_LED_BRIGHTNESS = 65` around `esp32-s3-touch-amoled-1.8-v2.cc:120`.
- Brightness clamp: min `5`, max `80` around `esp32-s3-touch-amoled-1.8-v2.cc:120-126`.
- Default color: `kWarmWhiteColor = {0, 0, 0, 255}` around `esp32-s3-touch-amoled-1.8-v2.cc:134-136`.
- Default warm-white is applied in `SetDefaultWarmWhite()` around `esp32-s3-touch-amoled-1.8-v2.cc:251-258`.
- `InitializeLedStrip()` creates the strip using `LUMI_LED_DATA_PIN` and `LUMI_LED_COUNT` around `esp32-s3-touch-amoled-1.8-v2.cc:671-673`.
- `GetLed()` returns `led_strip_` around `esp32-s3-touch-amoled-1.8-v2.cc:879`.

## 3. Functions that must remain intact

- AMOLED screen: `InitializeDisplay()` and `CustomLcdDisplay`, `esp32-s3-touch-amoled-1.8-v2.cc:365-384` and `736-771`.
- Touch/button:
  - Touch: `InitializeTouch()`, `esp32-s3-touch-amoled-1.8-v2.cc:773-817`.
  - Boot button click behavior: `InitializeButtons()`, `esp32-s3-touch-amoled-1.8-v2.cc:644-665`.
- Microphone and speaker:
  - `GetAudioCodec()` returns ES8311 codec, `esp32-s3-touch-amoled-1.8-v2.cc:867-872`.
  - ES8311 input/output device mode, `main/audio/codecs/es8311_audio_codec.cc:43-52`.
- Wi-Fi:
  - Board inherits `WifiBoard` at `esp32-s3-touch-amoled-1.8-v2.cc:405`.
  - `WifiBoard::StartNetwork()` initializes Wi-Fi manager around `main/boards/common/wifi_board.cc:52-80`.
  - `WifiBoard::GetBoardJson()` reports SSID/RSSI/IP/MAC around `main/boards/common/wifi_board.cc:268-281`.
- Auto-listen:
  - State fields around `esp32-s3-touch-amoled-1.8-v2.cc:416-421`.
  - `TryAutoStartListening()` calls `Application::StartListening(kListeningModeAutoStop)` around `esp32-s3-touch-amoled-1.8-v2.cc:444-467`.
  - `RunAutoStartListeningLoop()` starts after boot and restarts on idle around `esp32-s3-touch-amoled-1.8-v2.cc:470-505`.
  - `InitializeAutoStartChat()` creates task `auto_listen` around `esp32-s3-touch-amoled-1.8-v2.cc:667-669`.
  - `Application::StartListening(ListeningMode mode)` is declared at `main/application.h:99-100` and implemented around `main/application.cc:677-684`.
- vi-VN:
  - `CONFIG_LANGUAGE_VI_VN=y` from `sdkconfig:610`.
  - `main/CMakeLists.txt:897-898` maps selected language to `vi-VN`.
  - `main/assets/lang_config.h:1-14` states generated language config and `Lang::CODE = "vi-VN"`.
  - OTA sends `Accept-Language: Lang::CODE` in `main/ota.cc:82-84`.
  - MQTT hello and audio params include `Lang::CODE` around `main/protocols/mqtt_protocol.cc:297-337`.
  - Listen start payload includes language/locale/asr/tts fields around `main/protocols/protocol.cc:58-74`.
- Official MQTT/UDP/Opus/AES:
  - Protocol selection uses OTA config: `Application::InitializeProtocol()` around `main/application.cc:475-489`.
  - If OTA has MQTT config, firmware uses `MqttProtocol`; if no protocol, it falls back to MQTT, `main/application.cc:482-488`.
  - MQTT reads `endpoint`, `client_id`, `username`, `password`, `publish_topic` from NVS namespace `mqtt`, `main/protocols/mqtt_protocol.cc:65-72`.
  - MQTT hello requests `transport="udp"`, `version=3`, `format="opus"`, sample rate `16000`, channels `1`, frame duration `OPUS_FRAME_DURATION_MS`, `main/protocols/mqtt_protocol.cc:297-337`.
  - `OPUS_FRAME_DURATION_MS` is `60` in `main/audio/audio_service.h:39`.
  - Audio encode/decode uses ESP Opus APIs in `main/audio/audio_service.cc:66-82` and `327-445`.
  - UDP server/key/nonce are parsed from server hello around `main/protocols/mqtt_protocol.cc:371-385`.
  - AES-CTR encryption/decryption uses `mbedtls_aes_crypt_ctr()` around `main/protocols/mqtt_protocol.cc:166-189` and `267-281`.
  - AES key is set with 128-bit key around `main/protocols/mqtt_protocol.cc:383-385`.
- MCP/tool `self.led.control`:
  - Registered in the current board file around `esp32-s3-touch-amoled-1.8-v2.cc:829-847`.
  - Callback calls `HandleLedControl(properties)` around `esp32-s3-touch-amoled-1.8-v2.cc:845-847`.

## 4. Official cloud

- Final OTA URL: `https://api.tenclass.net/xiaozhi/ota/`.
- Source of URL:
  - Kconfig default: `main/Kconfig.projbuild:1-5`.
  - Current sdkconfig: `sdkconfig:601`.
  - `Ota::GetCheckVersionUrl()` falls back to `CONFIG_OTA_URL` when no NVS override exists, `main/ota.cc:46-59`.
- MQTT is obtained from OTA:
  - OTA response field `mqtt` is parsed and saved to `Settings("mqtt", true)` around `main/ota.cc:162-179`.
  - `Application::InitializeProtocol()` chooses `MqttProtocol` when `ota_->HasMqttConfig()` around `main/application.cc:475-489`.
  - `MqttProtocol::StartMqttClient()` reads MQTT settings from NVS around `main/protocols/mqtt_protocol.cc:65-72`.
- Do not use Lamp Cham server:
  - Current source explicitly removes stale NVS OTA override if it contains `lampcham.lamania.io.vn` or `ota.lampcham.lamania.io.vn`, `main/ota.cc:48-55`.
- Do not use custom WebSocket PCM16:
  - Current MQTT path is MQTT + UDP + Opus + AES-CTR as above.
  - WebSocket source, if selected by OTA, also uses Opus in hello (`main/protocols/websocket_protocol.cc:203-239`), not PCM16.
- Do not change endpoint by guesswork. OTA endpoint source is `CONFIG_OTA_URL` and NVS override handling in `main/ota.cc:46-59`.

## 5. Activation

Source behavior:

- `Device-Id` comes from `SystemInfo::GetMacAddress()` in `main/ota.cc:69-77`.
- `SystemInfo::GetMacAddress()` reads ESP MAC (`ESP_MAC_WIFI_STA` for this non-Ethernet ESP32-S3 path) around `main/system_info.cc:35-47`.
- `Client-Id` comes from `Board::GetInstance().GetUuid()` in `main/ota.cc:69-77`.
- `Board::GetUuid()` returns stored/generated UUID; constructor reads NVS namespace `board` key `uuid`, generates one only if empty, and saves it around `main/boards/common/board.cc:15-23`; UUID generation is around `main/boards/common/board.cc:25-45`; `GetUuid()` is in `main/boards/common/board.h:69`.
- `activation.code` is read only from server OTA JSON `activation.code`, `main/ota.cc:138-160`.
- Activation payload is generated only when server gives `activation.challenge`; `Ota::Activate()` exits if no challenge around `main/ota.cc:483-517`.
- No source path found that fakes an activation code, forces activation, changes MAC, or erases flash for activation.

Observed current runtime activation state:

- Existing local log `build/log/idf_py_stdout_output_15940:166-170` shows:
  - `http_status=200`
  - `activation_present=0`
  - `code_present=0`
  - `challenge_present=0`
  - `mqtt_present=1`
- Same log shows `Device-Id` and `Client-Id` at `build/log/idf_py_stdout_output_15940:158-159`, and MQTT connected at `build/log/idf_py_stdout_output_15940:174`.
- Treat this as evidence that the server currently sees this device as activated. Do not fake code, force activation, change MAC, or run `erase-flash` unless the task explicitly requests it.

## 6. MCP/tool: self.led.control

Verified tool in current board file: `main/boards/waveshare/esp32-s3-touch-amoled-1.8-v2/esp32-s3-touch-amoled-1.8-v2.cc`.

Registration:

- `InitializeTools()` begins around `esp32-s3-touch-amoled-1.8-v2.cc:820`.
- `mcp_server.AddTool("self.led.control", ...)` is around `esp32-s3-touch-amoled-1.8-v2.cc:829-847`.

Schema exactly as source defines it:

```cpp
PropertyList({
    Property("action", kPropertyTypeString, std::string("set_color")),
    Property("status", kPropertyTypeString, std::string("on")),
    Property("color", kPropertyTypeString, std::string("warm_white")),
    Property("brightness", kPropertyTypeInteger, 65, 0, 100),
    Property("step", kPropertyTypeInteger, 10, 1, 100),
})
```

Callback exactly as source wires it:

```cpp
[this](const PropertyList& properties) -> ReturnValue {
    return HandleLedControl(properties);
}
```

Actual callback behavior:

- `HandleLedControl()` is around `esp32-s3-touch-amoled-1.8-v2.cc:675-734`.
- It reads:
  - `action` as lowercase string
  - `status` as lowercase string
  - `color` as lowercase string
  - `brightness` as integer
  - `step` as integer
- `status == "off"` forces `action = "off"`.
- `status` must otherwise be `"on"`.
- Supported actions:
  - `off`
  - `on`
  - `set_color`
  - `set_brightness`
  - `increase_brightness`
  - `decrease_brightness`
- Supported colors come from `ParseRgbwwColor()` around `esp32-s3-touch-amoled-1.8-v2.cc:323-363`:
  - `warm_white`
  - `ww`
  - `white` normalized to `warm_white`
  - `trucchi_warm`
  - `error_color`
  - `red`
  - `green`
  - `blue`
  - `yellow`
  - `purple`
  - `cyan`
  - `#RRGGBB`
- Brightness is clamped to `5..80` by `ClampLedBrightness()` around `esp32-s3-touch-amoled-1.8-v2.cc:120-131`, even though schema accepts `0..100`.

Do not invent another schema for this tool.

## 7. Current custom changes and classification

`git diff --numstat` currently reports text diffs in 13 tracked files:

- `main/Kconfig.projbuild`: 1 insert, 1 delete.
- `main/application.cc`: 32 inserts, 6 deletes.
- `main/application.h`: 2 inserts.
- `main/boards/common/i2c_device.cc`: 80 inserts, 13 deletes.
- `main/boards/common/i2c_device.h`: 6 inserts, 1 delete.
- `main/boards/waveshare/esp32-s3-touch-amoled-1.8-v2/config.h`: 8 inserts, 4 deletes.
- `main/boards/waveshare/esp32-s3-touch-amoled-1.8-v2/esp32-s3-touch-amoled-1.8-v2.cc`: 581 inserts, 88 deletes.
- `main/mcp_server.cc`: 304 inserts, 155 deletes.
- `main/ota.cc`: 29 inserts, 4 deletes.
- `main/protocols/mqtt_protocol.cc`: 23 inserts.
- `main/protocols/protocol.cc`: 6 inserts.
- `main/protocols/websocket_protocol.cc`: 23 inserts.
- `main/settings.cc`: 1 insert.

`git status --short` also reports `main/protocols/mqtt_protocol.h` modified, but current `git diff -- main/protocols/mqtt_protocol.h` and cached diff are empty; likely line-ending/metadata only unless later commands show otherwise.

Untracked items:

- `.codex-tmp-xiaozhi-server/`
- `CLIENT_REPORT.md`
- `FINAL_SERVER_HANDOFF.md`
- `SERVER_INTEGRATION_BLOCKERS.md`

Need to keep:

- Board selection and language:
  - `main/Kconfig.projbuild`: default language changed to `LANGUAGE_VI_VN`.
  - `sdkconfig` currently already selects `CONFIG_LANGUAGE_VI_VN=y`.
- Board hardware/custom features:
  - `config.h`: adds `LUMI_LED_DATA_PIN GPIO42` and `LUMI_LED_COUNT 30`.
  - Board file: `Sk6812RgbwwStrip`, default warm-white, `GetLed()`, `InitializeLedStrip()`.
  - Board file: `self.led.control` registration and `HandleLedControl()`.
  - Board file + application overload: auto-listen requires `Application::StartListening(ListeningMode mode)` and `pending_start_listening_mode_`.
- Official cloud path:
  - `main/ota.cc`: stale Lamp Cham OTA override removal and fallback to `CONFIG_OTA_URL`.
  - `main/settings.cc`: `EraseKey()` sets `dirty_ = true`, needed so erased stale OTA override can persist.
- vi-VN protocol propagation:
  - `main/protocols/mqtt_protocol.cc`, `main/protocols/websocket_protocol.cc`, and `main/protocols/protocol.cc` add `Lang::CODE` fields.

Temporary diagnostics:

- `main/ota.cc`: `[ACT_DIAG]` logs for device id, client id, activation version, HTTP status, activation/code/challenge/mqtt presence.
- `main/application.cc`: activation code logs in `CheckNewVersion()` and `ShowActivationCode()`.
- `main/protocols/mqtt_protocol.cc` and `main/protocols/websocket_protocol.cc`: `[LANG] hello payload`, language summary, and server config logs.
- Existing `build/log/idf_py_stdout_output_15940` contains useful activation diagnostics, but logs are not source.

Can be reverted if not explicitly needed:

- Formatting-only reorder/spacing changes in board file, config file, MCP file, and comments.
- Untracked reports (`CLIENT_REPORT.md`, `FINAL_SERVER_HANDOFF.md`, `SERVER_INTEGRATION_BLOCKERS.md`) should not be treated as source of truth.
- `.codex-tmp-xiaozhi-server/` is a separate untracked server checkout/copy and is not firmware source.

Unclear; user/maintainer should decide before changing:

- `main/boards/common/i2c_device.cc/.h`: global I2C changes lower speed to 100 kHz and replace `ESP_ERROR_CHECK` with checked/logged methods. This may help PMIC boot stability but affects common I2C behavior across boards.
- `main/mcp_server.cc`: added/expanded `self.audio.volume`, deprecated alias behavior, and unsupported `self.music.play`/`self.music.stop` tools. The music tools return unsupported messages; verify whether these should remain, because this document's required tool is only `self.led.control`.
- Board PMIC changes: `InitializeAxp2101()` now probes/retries and allows `pmic_ == nullptr`; likely stability-related, but keep only if it matches hardware behavior.

## 8. Rules for future source edits

- Current source/config is the highest source of truth.
- Do not use old reports as truth when they conflict with current source.
- Do not refactor outside the task.
- Do not change dependency versions or ESP-IDF version.
- Do not edit CMake, Kconfig, partitions, or sdkconfig unless the task truly requires it.
- Touch the fewest files necessary.
- Do not add fake APIs, unsupported pseudo-tools, or pseudocode.
- Do not change the OTA/MQTT endpoint by guessing.
- Do not use Lamp Cham server or custom WebSocket PCM16.
- Do not fake activation code, force activation, change MAC, or erase flash unless the task explicitly asks for that operation.
- Keep AMOLED, touch/button, ES8311 mic/speaker, Wi-Fi, auto-listen, vi-VN, official MQTT/UDP/Opus/AES, and `self.led.control`.
