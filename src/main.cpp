// Stack-chan Calendar Reminder for M5Stack Core Gray (Core1).
//
// This app turns Stack-chan into a desktop calendar reminder:
//   * connects to Wi-Fi and keeps time via NTP,
//   * downloads a remote calendar (iCalendar / .ics),
//   * announces upcoming events with a chime, an LED flash and a speech
//     balloon on the avatar,
//   * shows a Wi-Fi status indicator on the avatar screen,
//   * exposes an HTTP control API to query/refresh the calendar, change the
//     volume, trigger a test reminder, make Stack-chan speak and update the
//     firmware over the air.
//
// The servo control and the Bluetooth speaker (A2DP) audio code that the
// original sketch shipped with have both been removed.
//
// Based on the Bluetooth_with_ESP32A2DP example from M5Unified.
// Copyright (c) 2022 Takao Akaki

#include <Arduino.h>

#include <SD.h>
#include <WiFi.h>
#include <M5Unified.h>
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

bool calendar_services_running = false;
unsigned long last_calendar_poll_millis = 0;

// リマインダー表示を自動で消すためのタイマー
const unsigned long reminder_display_ms = 30000;
unsigned long reminder_shown_millis = 0;
bool reminder_showing = false;

// Wi-Fi status indicator (drawn on the avatar screen).
const unsigned long wifi_indicator_interval = 2000;
unsigned long last_wifi_indicator_millis = 0;
int last_wifi_indicator_state = -99;  // force first draw

ColorPalette *cps;

/// set M5Speaker virtual channel (0-7)
static constexpr uint8_t m5spk_virtual_channel = 0;

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
// カレンダー/APIサービスの開始（Wi-Fi接続・時刻同期・取得・API開始）
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

// --------------------------------------------------------------------------
// Wi-Fi status indicator (top-left corner of the avatar screen)
// --------------------------------------------------------------------------
// Mirrors where the avatar keeps the persistent battery icon (top-right), so
// the corner is not overwritten by the face animation. Drawn inside a display
// transaction, only when the state changes or on a periodic refresh.
void drawWifiIndicator(bool force) {
  bool connected = (WiFi.status() == WL_CONNECTED);
  int rssi = connected ? WiFi.RSSI() : 0;
  int bars = 0;
  if (connected) {
    if (rssi >= -55)      bars = 4;
    else if (rssi >= -65) bars = 3;
    else if (rssi >= -75) bars = 2;
    else                  bars = 1;
  }
  // Encode the whole visible state in one int so we only repaint on change.
  int state = connected ? bars : (calendar_services_running ? 0 : -1);
  if (!force && state == last_wifi_indicator_state) return;
  last_wifi_indicator_state = state;

  const int x0 = 6;    // left margin
  const int y0 = 6;    // top margin
  const int bw = 5;    // bar width
  const int gap = 2;   // gap between bars
  const int base_y = y0 + 20;
  const uint16_t dim = 0x39E7;  // dark gray for empty bars
  const uint16_t on_color = connected ? TFT_GREEN : TFT_RED;

  M5.Display.startWrite();
  M5.Display.fillRect(x0 - 2, y0 - 2, 4 * (bw + gap) + 4, 26, TFT_BLACK);
  for (int i = 0; i < 4; i++) {
    int h = 5 + i * 4;                 // taller bars to the right
    int bx = x0 + i * (bw + gap);
    int by = base_y - h;
    uint16_t c = (i < bars) ? on_color : dim;
    M5.Display.fillRect(bx, by, bw, h, c);
  }
  if (!connected) {
    // Red slash to make "no link" unmistakable.
    M5.Display.drawLine(x0 - 1, y0 - 1, x0 + 4 * (bw + gap), base_y, TFT_RED);
    M5.Display.drawLine(x0 - 1, y0, x0 + 4 * (bw + gap), base_y + 1, TFT_RED);
  }
  M5.Display.endWrite();
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

  // start_volume はSC_BasicConfig.yamlのbluetooth設定から流用する（音量値のみ）。
  M5.Speaker.setVolume(system_config.getBluetoothSetting()->start_volume);
  M5.Speaker.setChannelVolume(system_config.getBluetoothSetting()->start_volume, m5spk_virtual_channel);

  // サーボを廃止したので、電源は常に横のコネクタから給電する。
  M5.Power.setExtOutput(true);

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

#ifdef USE_LED
  FastLED.addLeds<SK6812, LED_PIN, GRB>(leds, NUM_LEDS);  // GRB ordering is typical
  FastLED.setBrightness(32);
  level_led(5, 5);
  delay(1000);
  turn_off_led();
#endif

  // Connect Wi-Fi, sync time, fetch the calendar and start the control API.
  startCalendarServices();
  drawWifiIndicator(true);
}

void loop(void)
{
  M5.update();

  if (M5.BtnA.wasPressed()) {
    // BtnAでリモートカレンダーを手動で再取得する。
    avatar.setExpression(Expression::Doubt);
    avatar.setSpeechText("Refreshing...");
    M5.Speaker.tone(1000, 80);
    bool ok = calendar.refresh();
    last_calendar_poll_millis = millis();
    avatar.setExpression(Expression::Neutral);
    avatar.setSpeechText(ok ? "Calendar updated" : "Refresh failed");
    reminder_showing = true;               // reuse the auto-clear timer
    reminder_shown_millis = millis();
    drawWifiIndicator(true);
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

  // --- カレンダー/APIの定常処理 ---
  if (calendar_services_running) {
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

  // Wi-Fi status indicator: repaint periodically (redraws only on change).
  if ((millis() - last_wifi_indicator_millis) > wifi_indicator_interval) {
    drawWifiIndicator(false);
    last_wifi_indicator_millis = millis();
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
