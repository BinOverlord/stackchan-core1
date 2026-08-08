/*
  CalendarConfig.hpp

  Reads the Wi-Fi, remote calendar, NTP and control-API settings for the
  calendar reminder feature out of the single firmware config file
  (/yaml/SC_Config.yaml). Only the wifi / calendar / time / reminder / api
  sections are used here; the stackchan-arduino loader reads the rest of the
  same file.

  A tiny purpose built line reader is used instead of a full YAML library so the
  feature stays self contained. The expected structure is a two level tree with
  2-space indentation and `key: value` pairs (see SC_Config.yaml).
*/
#ifndef CALENDAR_CONFIG_HPP_
#define CALENDAR_CONFIG_HPP_

#include <Arduino.h>
#include <FS.h>

struct CalendarConfig {
  // wifi
  String wifi_ssid = "";
  String wifi_password = "";
  uint32_t wifi_connect_timeout_sec = 20;

  // calendar
  String ics_url = "";
  uint32_t poll_interval_sec = 300;
  uint32_t reminder_lead_sec = 300;
  uint16_t max_events = 20;

  // time
  String ntp_server = "pool.ntp.org";
  long gmt_offset_sec = 0;
  int daylight_offset_sec = 0;

  // reminder
  bool tone_enabled = true;
  bool speak_enabled = true;
  uint8_t chime_repeat = 2;
  String sound_file = "";
  bool led_flash = true;

  // api
  bool api_enabled = true;
  uint16_t api_port = 80;
  String api_auth_token = "";

  bool loaded = false;

  bool isWifiConfigured() const {
    return wifi_ssid.length() > 0;
  }
  bool isCalendarConfigured() const {
    return ics_url.length() > 0;
  }
};

class CalendarConfigLoader {
public:
  // Returns true if the file was found and parsed.
  static bool load(fs::FS &fs, const char *path, CalendarConfig &cfg) {
    File file = fs.open(path, FILE_READ);
    if (!file) {
      Serial.printf("[CalendarConfig] %s not found, using defaults.\n", path);
      return false;
    }

    String section = "";
    while (file.available()) {
      String line = file.readStringUntil('\n');
      parseLine(line, section, cfg);
    }
    file.close();
    cfg.loaded = true;
    Serial.println("[CalendarConfig] loaded.");
    return true;
  }

private:
  static void parseLine(String line, String &section, CalendarConfig &cfg) {
    // Strip trailing CR and comments (# ... ) that are not inside a quote.
    line.replace("\r", "");
    int hash = findCommentStart(line);
    if (hash >= 0) line = line.substring(0, hash);

    // Count leading spaces to know the nesting level.
    int indent = 0;
    while (indent < (int)line.length() && line[indent] == ' ') indent++;
    String trimmed = line.substring(indent);
    trimmed.trim();
    if (trimmed.length() == 0) return;

    int colon = trimmed.indexOf(':');
    if (colon < 0) return;
    String key = trimmed.substring(0, colon);
    key.trim();
    String value = trimmed.substring(colon + 1);
    value.trim();
    value = unquote(value);

    if (value.length() == 0) {
      // A section header such as `wifi:` (top level, no indentation).
      if (indent == 0) section = key;
      return;
    }

    apply(section, key, value, cfg);
  }

  static void apply(const String &section, const String &key,
                    const String &value, CalendarConfig &cfg) {
    if (section == "wifi") {
      if (key == "ssid") cfg.wifi_ssid = value;
      else if (key == "password") cfg.wifi_password = value;
      else if (key == "connect_timeout_sec") cfg.wifi_connect_timeout_sec = value.toInt();
    } else if (section == "calendar") {
      if (key == "ics_url") cfg.ics_url = value;
      else if (key == "poll_interval_sec") cfg.poll_interval_sec = value.toInt();
      else if (key == "reminder_lead_sec") cfg.reminder_lead_sec = value.toInt();
      else if (key == "max_events") cfg.max_events = value.toInt();
    } else if (section == "time") {
      if (key == "ntp_server") cfg.ntp_server = value;
      else if (key == "gmt_offset_sec") cfg.gmt_offset_sec = value.toInt();
      else if (key == "daylight_offset_sec") cfg.daylight_offset_sec = value.toInt();
    } else if (section == "reminder") {
      if (key == "tone_enabled") cfg.tone_enabled = toBool(value);
      else if (key == "speak_enabled") cfg.speak_enabled = toBool(value);
      else if (key == "chime_repeat") cfg.chime_repeat = value.toInt();
      else if (key == "sound_file") cfg.sound_file = value;
      else if (key == "led_flash") cfg.led_flash = toBool(value);
    } else if (section == "api") {
      if (key == "enabled") cfg.api_enabled = toBool(value);
      else if (key == "port") cfg.api_port = value.toInt();
      else if (key == "auth_token") cfg.api_auth_token = value;
    }
  }

  // Find the start of a `#` comment, ignoring `#` inside double quotes.
  static int findCommentStart(const String &line) {
    bool in_quote = false;
    for (int i = 0; i < (int)line.length(); i++) {
      char c = line[i];
      if (c == '"') in_quote = !in_quote;
      else if (c == '#' && !in_quote) return i;
    }
    return -1;
  }

  static String unquote(String v) {
    v.trim();
    if (v.length() >= 2 && v[0] == '"' && v[v.length() - 1] == '"') {
      v = v.substring(1, v.length() - 1);
    }
    return v;
  }

  static bool toBool(const String &v) {
    String s = v;
    s.toLowerCase();
    return (s == "true" || s == "1" || s == "yes" || s == "on");
  }
};

#endif  // CALENDAR_CONFIG_HPP_
