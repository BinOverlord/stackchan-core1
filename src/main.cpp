// Stack-chan Calendar Reminder for M5Stack Core Gray (Core1).
//
// This app turns Stack-chan into a desktop calendar reminder:
//   * connects to Wi-Fi and keeps time via NTP,
//   * downloads a remote calendar (iCalendar / .ics),
//   * announces upcoming events with a chime, an LED flash and a speech
//     balloon on the avatar,
//   * exposes an HTTP control API to query/refresh the calendar, change the
//     volume, trigger a test reminder and make Stack-chan speak.
//
// It also keeps the original Bluetooth speaker mode. The servo control that the
// original sketch shipped with has been removed (this build has no servo).
//
// Based on the Bluetooth_with_ESP32A2DP example from M5Unified.
// Copyright (c) 2022 Takao Akaki

#include <Arduino.h>

#include <SD.h>
#include <M5Unified.h>
#include "BluetoothA2DPSink_M5Speaker.hpp"
#include "Avatar.h"

#include "CalendarConfig.hpp"
#include "CalendarClient.hpp"
#include "ControlAPI.hpp"

using namespace m5avatar;
Avatar avatar;

#include "Stackchan_system_config.h"

// M5GoBottomのLEDを使わない場合は下記の1行をコメントアウトしてください。
#define USE_LED
//#define USE_LED_OUT

#ifdef USE_LED
  #include <FastLED.h>
  #define NUM_LEDS 10
  #define NUM_LED_OUT 55
#if defined(ARDUINO_M5STACK_Core2)
  // M5Core2 + M5GoBottom2の組み合わせ
  #define LED_PIN 25
#else
  // M5Stack Core Gray/Basic/Fire + M5GoBottom1の組み合わせ
  #define LED_PIN 15
#endif
  CRGB leds[NUM_LEDS];
  #ifdef USE_LED_OUT
  CRGB leds_out[NUM_LED_OUT];
  #endif

  CHSV red (0, 255, 255);
  CHSV green (95, 255, 255);
  CHSV blue (160, 255, 255);
  CHSV magenta (210, 255, 255);
  CHSV yellow (45, 255, 255);
  CHSV hsv_table[5] = { blue, green, yellow, magenta, red };
  CHSV hsv_table_out[5] = { blue, green, yellow, magenta, red }; //red, magenta, yellow, green, blue };

  void turn_off_led() {
    // Now turn the LED off, then pause
    for(int i=0;i<NUM_LEDS;i++) leds[i] = CRGB::Black;
    FastLED.show();
  }

  void clear_led_buff() {
    // Now turn the LED off, then pause
    for(int i=0;i<NUM_LEDS;i++) leds[i] =  CRGB::Black;
  }

  void level_led(int level1, int level2) {
  if(level1 > 5) level1 = 5;
  if(level2 > 5) level2 = 5;

    clear_led_buff();
    for(int i=0;i<level1;i++){
      fill_gradient(leds, 0, hsv_table[i], 4, hsv_table[0] );
      #ifdef USE_LED_OUT
      fill_gradient(leds_out, 0, hsv_table_out[0], 18, hsv_table_out[i] );
      #endif
    }
    for(int i=0;i<level2;i++){
      fill_gradient(leds, 5, hsv_table[0], 9, hsv_table[i] );
      #ifdef USE_LED_OUT
      fill_gradient(leds_out, 19, hsv_table_out[i], 36, hsv_table_out[0] );
      #endif
    }
    FastLED.show();
  }
#endif



fs::FS json_fs = SD; // JSONファイルの収納場所(SPIFFS or SD)
StackchanSystemConfig system_config;
const char* stackchan_system_config_yaml = "/yaml/SC_Config.yaml";

// Calendar connection settings live in a separate YAML so SC_BasicConfig.yaml
// keeps controlling the avatar / speaker / LED / bluetooth as before.
CalendarConfig calendar_config;
const char* calendar_config_yaml = "/yaml/SC_CalendarConfig.yaml";
CalendarClient calendar;
ControlAPI control_api;

const unsigned long powericon_interval = 3000;  // バッテリーアイコンを更新する間隔(msec)
unsigned long last_powericon_millis = 0;

bool bluetooth_mode = false;
bool calendar_services_running = false;
unsigned long last_calendar_poll_millis = 0;

// リマインダー表示を自動で消すためのタイマー
const unsigned long reminder_display_ms = 30000;
unsigned long reminder_shown_millis = 0;
bool reminder_showing = false;

// --------------------
// Avatar関連の初期設定
#define LIPSYNC_LEVEL_MAX 10.0f
static float lipsync_level_max = LIPSYNC_LEVEL_MAX; // リップシンクの上限初期値
float mouth_ratio = 0.0f;
bool sing_happy = true;
ColorPalette *cps;
// Avatar関連の設定 end
// --------------------

uint32_t last_discharge_time = 0;  // USB給電が止まったときの時間(msec)

/// set M5Speaker virtual channel (0-7)
static constexpr uint8_t m5spk_virtual_channel = 0;

static BluetoothA2DPSink_M5Speaker a2dp_sink = { &M5.Speaker, m5spk_virtual_channel };
static fft_t fft;
static constexpr size_t WAVE_SIZE = 320;
static int16_t raw_data[WAVE_SIZE * 2];

void lipSync(void *args)
{
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  while (avatar->isDrawing())
  {
    uint64_t level = 0;
    auto buf = a2dp_sink.getBuffer();
    if (buf) {
#ifdef USE_LED
      // buf[0]: LEFT
      // buf[1]: RIGHT
      switch(system_config.getLedLR()) {
        case 1: // Left Only
          level_led(abs(buf[0])*10/INT16_MAX,abs(buf[0])*10/INT16_MAX);
          break;
        case 2: // Right Only
          level_led(abs(buf[1])*10/INT16_MAX,abs(buf[1])*10/INT16_MAX);
          break;
        default: // Stereo
          level_led(abs(buf[1])*10/INT16_MAX,abs(buf[0])*10/INT16_MAX);
          break;
      }
#endif

      memcpy(raw_data, buf, WAVE_SIZE * 2 * sizeof(int16_t));
      fft.exec(raw_data);
      for (size_t bx = 5; bx <= 60; ++bx) { // リップシンクで抽出する範囲はここで指定(低音)0〜64（高音）
        int32_t f = fft.get(bx);
        level += abs(f);
      }
    }

    mouth_ratio = (float)(level >> 16)/lipsync_level_max;
    if (mouth_ratio > 1.2f) {
      if (mouth_ratio > 1.5f) {
        lipsync_level_max += 10.0f; // リップシンク上限を大幅に超えるごとに上限を上げていく。
      }
      mouth_ratio = 1.2f;
    }
    avatar->setMouthOpenRatio(mouth_ratio);
    vTaskDelay(30/portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL);
}

void hvt_event_callback(int avatar_expression, const char* text) {
  avatar.setExpression((Expression)avatar_expression);
  avatar.setSpeechText(text);
}

void avrc_metadata_callback(uint8_t data1, const uint8_t *data2)
{
  Serial.printf("AVRC metadata rsp: attribute id 0x%x, %s\n", data1, data2);
  if (sing_happy) {
    avatar.setExpression(Expression::Happy);
  } else {
    avatar.setExpression(Expression::Neutral);
  }
  sing_happy = !sing_happy;

}

// --------------------------------------------------------------------------
// リマインダーの音・演出
// --------------------------------------------------------------------------
void playReminderChime() {
  // 3音のチャイムを設定回数だけ鳴らす。
  uint8_t repeat = calendar_config.chime_repeat;
  if (repeat == 0) repeat = 1;
  for (uint8_t r = 0; r < repeat; r++) {
    M5.Speaker.tone(880.0, 150);   // A5
    delay(180);
    M5.Speaker.tone(1174.0, 150);  // D6
    delay(180);
    M5.Speaker.tone(1568.0, 300);  // G6
    delay(350);
  }
}

// SDカードにWAVファイルが指定されていれば再生する。無ければチャイムを鳴らす。
void playReminderSound() {
  if (!calendar_config.tone_enabled) return;

  if (calendar_config.sound_file.length() > 0 && SD.exists(calendar_config.sound_file)) {
    File f = SD.open(calendar_config.sound_file, FILE_READ);
    if (f) {
      size_t sz = f.size();
      // Core Gray has no PSRAM, so keep the reminder sound small.
      if (sz > 0 && sz <= 300 * 1024) {
        uint8_t *buf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_8BIT);
        if (buf) {
          f.read(buf, sz);
          f.close();
          M5.Speaker.playWav(buf, sz, 1, m5spk_virtual_channel, true);
          while (M5.Speaker.isPlaying(m5spk_virtual_channel)) { delay(10); }
          heap_caps_free(buf);
          return;
        }
      }
      f.close();
    }
  }
  playReminderChime();
}

#ifdef USE_LED
void reminderLedFlash() {
  if (!calendar_config.led_flash) return;
  for (int i = 0; i < 3; i++) {
    level_led(5, 5);
    delay(150);
    turn_off_led();
    delay(150);
  }
}
#else
void reminderLedFlash() {}
#endif

void showReminder(const String &title) {
  avatar.setExpression(Expression::Happy);
  if (calendar_config.speak_enabled) {
    String text = "Reminder: " + title;
    avatar.setSpeechText(text.c_str());
  }
  reminder_showing = true;
  reminder_shown_millis = millis();
  reminderLedFlash();
  playReminderSound();
}

void onReminder(const CalendarEvent &ev) {
  Serial.printf("[Reminder] %s\n", ev.title.c_str());
  showReminder(ev.title);
}

// --------------------------------------------------------------------------
// カレンダー/APIサービスの開始・停止（Bluetoothモードと排他運用）
// --------------------------------------------------------------------------
void startCalendarServices() {
  if (calendar_services_running) return;
  if (!calendar_config.isWifiConfigured()) {
    Serial.println("[Calendar] Wi-Fi not configured; calendar disabled.");
    return;
  }
  avatar.setSpeechText("Connecting WiFi...");
  if (!calendar.connectWiFi()) {
    avatar.setSpeechText("WiFi failed");
    return;
  }
  calendar.syncTime();
  calendar.refresh();
  last_calendar_poll_millis = millis();

  if (calendar_config.api_enabled) {
    control_api.begin(&calendar, calendar_config.api_port, calendar_config.api_auth_token);
  }
  calendar_services_running = true;
  avatar.setSpeechText("Calendar Ready");
}

void stopCalendarServices() {
  if (!calendar_services_running) return;
  control_api.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  calendar_services_running = false;
}

void enterBluetoothMode() {
  if (bluetooth_mode) return;
  // Wi-Fi and Bluetooth Classic fight over the radio, so shut calendar down.
  stopCalendarServices();
  a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
  a2dp_sink.setHvtEventCallback(hvt_event_callback);
  a2dp_sink.start(system_config.getBluetoothSetting()->device_name.c_str(), true);
  avatar.setExpression(Expression::Sad);
  avatar.setSpeechText("Bluetooth Mode");
  M5.Speaker.tone(1000, 100);
  bluetooth_mode = true;
}

void exitBluetoothMode() {
  if (!bluetooth_mode) return;
  avatar.setExpression(Expression::Neutral);
  avatar.setSpeechText("Calendar Mode");
  M5.Speaker.tone(800, 100);
  a2dp_sink.stop();
  a2dp_sink.end(true);
  delay(1000);
  bluetooth_mode = false;
  startCalendarServices();
}

void avatarStart() {
  avatar.start();
  avatar.addTask(lipSync, "lipSync");
}
void avatarStop() {
  avatar.stop();
}

void setup(void)
{
  auto cfg = M5.config();
#ifndef ARDUINO_M5STACK_Core2
  cfg.output_power = true;
#endif

  M5.begin(cfg);

  { /// custom setting
    auto spk_cfg = M5.Speaker.config();
    /// Increasing the sample_rate will improve the sound quality instead of increasing the CPU load.
#ifdef BOARD_HAS_PSRAM
    // PSRAM搭載機種(M5Stack Fire/Core2/AWS)向けのパラメータ
    spk_cfg.sample_rate = 96000; // default:64000 (64kHz)  e.g. 48000 , 50000 , 80000 , 96000 , 100000 , 128000 , 144000 , 192000 , 200000
    spk_cfg.task_pinned_core = APP_CPU_NUM;
    spk_cfg.dma_buf_count = 20;
    spk_cfg.dma_buf_len = 256;
#else
    // M5Stack Core Gray/Basic/Go(PSRAM無し)向けのパラメータ
    // 音が途切れる場合はdma_buf_countとdma_buf_lenを増やすと改善できる場合あり。
    // 顔が表示されなかったり、点滅するようであれば増やし過ぎなので減らしてください。
    spk_cfg.sample_rate = 64000; // default:64000 (64kHz)
    spk_cfg.task_pinned_core = APP_CPU_NUM;
    spk_cfg.dma_buf_count = 10;
    spk_cfg.dma_buf_len = 192;
#endif
    M5.Speaker.config(spk_cfg);
  }


  M5.Speaker.begin();

  // BASICとGRAYのV2.6で25MHzだと読み込めないため15MHzまで下げています。
  SD.begin(GPIO_NUM_4, SPI, 15000000);

  delay(1000);
  system_config.loadConfig(json_fs, stackchan_system_config_yaml);
  // カレンダー接続用の設定ファイルを読み込む。
  CalendarConfigLoader::load(json_fs, calendar_config_yaml, calendar_config);

  M5.Speaker.setVolume(system_config.getBluetoothSetting()->start_volume);
  M5.Speaker.setChannelVolume(system_config.getBluetoothSetting()->start_volume, m5spk_virtual_channel);

  // サーボを廃止したので、電源は常に横のコネクタから給電する。
  M5.Power.setExtOutput(true);

  bluetooth_mode = system_config.getBluetoothSetting()->starting_state;
  Serial.printf("Bluetooth_mode:%s\n", bluetooth_mode ? "true" : "false");

  // サーボを使わないのでバッテリーアイコンは常に表示する。
  avatar.setBatteryIcon(true);
  avatar.setBatteryStatus(M5.Power.isCharging(), M5.Power.getBatteryLevel());

  avatar.init(1); // start drawing
  cps = new ColorPalette();

  // Avatarの色を変えたい場合は下記の2行のカラーコードを書き換えてください。
  cps->set(COLOR_PRIMARY, TFT_WHITE);
  cps->set(COLOR_BACKGROUND, TFT_BLACK);
  avatar.setColorPalette(*cps);
  last_powericon_millis = millis();

  avatar.addTask(lipSync, "lipSync");
  avatar.setExpression(Expression::Neutral);
  avatar.setSpeechFont(system_config.getFont());

  // Wire the control API hooks to the device.
  calendar.begin(calendar_config);
  calendar.onReminder(onReminder);
  control_api.setVolume = [](uint8_t v) {
    M5.Speaker.setVolume(v);
    M5.Speaker.setChannelVolume(m5spk_virtual_channel, v);
  };
  control_api.getVolume = []() -> uint8_t {
    return M5.Speaker.getChannelVolume(m5spk_virtual_channel);
  };
  control_api.speak = [](const String &text, int expression) {
    avatar.setExpression((Expression)expression);
    avatar.setSpeechText(text.c_str());
  };
  control_api.testReminder = []() { showReminder("Test reminder"); };
  control_api.refreshCalendar = []() -> bool { return calendar.refresh(); };
  control_api.bluetoothMode = []() -> bool { return bluetooth_mode; };

#ifdef USE_LED
  FastLED.addLeds<SK6812, LED_PIN, GRB>(leds, NUM_LEDS);  // GRB ordering is typical
  FastLED.setBrightness(32);
  level_led(5, 5);
  delay(1000);
  turn_off_led();
#endif

  if (bluetooth_mode) {
    a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
    a2dp_sink.setHvtEventCallback(hvt_event_callback);
    a2dp_sink.start(system_config.getBluetoothSetting()->device_name.c_str(), true);
    avatar.setExpression(Expression::Sad);
    avatar.setSpeechText("Bluetooth Mode");
  } else {
    // Normal (calendar) mode: connect Wi-Fi, sync time, fetch calendar, API.
    startCalendarServices();
  }
}

void loop(void)
{
  M5.update();

  if (M5.BtnA.wasDecideClickCount())
  {
    switch (M5.BtnA.getClickCount())
    {
    case 1:
      // シングルクリックでBluetoothスピーカーモードへ。
      enterBluetoothMode();
      break;

    case 2:
      // ダブルクリックでカレンダーモードへ戻る。
      exitBluetoothMode();
      break;
    }
  }
  if (M5.BtnB.wasPressed()) {
    uint8_t volume = M5.Speaker.getChannelVolume(m5spk_virtual_channel);
    volume = volume - 10;
    M5.Speaker.setVolume(volume);
    M5.Speaker.setChannelVolume(m5spk_virtual_channel, volume);
    M5.Speaker.tone(2000, 100);
    delay(200);
    M5.Speaker.tone(1000, 100);
  }
  if (M5.BtnC.wasPressed()) {
    uint8_t volume = M5.Speaker.getChannelVolume(m5spk_virtual_channel);
    volume = volume + 10;
    M5.Speaker.setVolume(volume);
    M5.Speaker.setChannelVolume(m5spk_virtual_channel, volume);
    M5.Speaker.tone(1000, 100);
    delay(200);
    M5.Speaker.tone(2000, 100);
  }

  // --- カレンダー/APIの定常処理（Bluetoothモード時は動かさない） ---
  if (!bluetooth_mode && calendar_services_running) {
    control_api.handleClient();
    calendar.update();

    // 定期的にリモートカレンダーを再取得する。
    unsigned long poll_ms = (unsigned long)calendar_config.poll_interval_sec * 1000UL;
    if (poll_ms > 0 && (millis() - last_calendar_poll_millis) > poll_ms) {
      calendar.refresh();
      last_calendar_poll_millis = millis();
    }

    // リマインダー表示を一定時間で消す。
    if (reminder_showing && (millis() - reminder_shown_millis) > reminder_display_ms) {
      avatar.setExpression(Expression::Neutral);
      avatar.setSpeechText("");
      reminder_showing = false;
    }
  }

  if ((millis() - last_powericon_millis) > powericon_interval) {
    avatar.setBatteryStatus(M5.Power.isCharging(), M5.Power.getBatteryLevel());
    last_powericon_millis = millis();
  }
}

#if !defined ( ARDUINO )
extern "C" {
  void loopTask(void*)
  {
    setup();
    for (;;) {
      loop();
    }
    vTaskDelete(NULL);
  }

  void app_main()
  {
    xTaskCreatePinnedToCore(loopTask, "loopTask", 8192, NULL, 1, NULL, 1);
  }
}
#endif
