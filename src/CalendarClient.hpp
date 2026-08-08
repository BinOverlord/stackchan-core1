/*
  CalendarClient.hpp

  Connects to Wi-Fi, keeps the clock in sync via NTP, downloads a remote
  calendar in iCalendar (.ics) format and schedules reminders for the upcoming
  events. When a reminder becomes due the registered callback is invoked so the
  main sketch can play a sound and show the event in the avatar balloon.

  The ICS feed is streamed and parsed line by line (with RFC 5545 line
  unfolding) to keep memory usage low on the PSRAM-less M5Stack Core Gray.
*/
#ifndef CALENDAR_CLIENT_HPP_
#define CALENDAR_CLIENT_HPP_

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <functional>
#include <vector>
#include <algorithm>
#include <time.h>

#include "CalendarConfig.hpp"

struct CalendarEvent {
  String title;
  time_t start_epoch = 0;   // event start, UTC epoch seconds
  time_t reminder_epoch = 0;  // when the reminder should fire, UTC epoch seconds
  bool reminder_fired = false;
};

class CalendarClient {
public:
  typedef std::function<void(const CalendarEvent &)> ReminderCallback;

  void begin(const CalendarConfig &cfg) { _cfg = cfg; }

  void onReminder(ReminderCallback cb) { _cb = cb; }

  const std::vector<CalendarEvent> &events() const { return _events; }
  bool wifiConnected() const { return WiFi.status() == WL_CONNECTED; }
  bool timeSynced() const { return _time_synced; }
  const String &lastError() const { return _last_error; }
  time_t lastRefresh() const { return _last_refresh; }

  // Attempt to (re)connect to Wi-Fi. Non blocking-ish: waits up to the
  // configured timeout. Returns true if connected.
  bool connectWiFi() {
    if (!_cfg.isWifiConfigured()) {
      _last_error = "wifi not configured";
      return false;
    }
    if (wifiConnected()) return true;

    Serial.printf("[Calendar] connecting to Wi-Fi '%s'...\n", _cfg.wifi_ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(_cfg.wifi_ssid.c_str(), _cfg.wifi_password.c_str());

    uint32_t deadline = millis() + _cfg.wifi_connect_timeout_sec * 1000UL;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
      delay(250);
    }
    if (wifiConnected()) {
      Serial.printf("[Calendar] Wi-Fi connected, IP: %s\n", WiFi.localIP().toString().c_str());
      _last_error = "";
      return true;
    }
    _last_error = "wifi connect failed";
    Serial.println("[Calendar] Wi-Fi connect failed.");
    return false;
  }

  // Kick off NTP sync. Returns true once the clock looks valid.
  bool syncTime() {
    if (!wifiConnected()) return false;
    configTime(_cfg.gmt_offset_sec, _cfg.daylight_offset_sec, _cfg.ntp_server.c_str());
    // Wait briefly for the first sync (epoch jumps well past 2021 when valid).
    for (int i = 0; i < 20; i++) {
      if (time(nullptr) > 1609459200) {  // 2021-01-01
        _time_synced = true;
        _last_error = "";
        return true;
      }
      delay(250);
    }
    _last_error = "ntp sync failed";
    return false;
  }

  // Download the remote calendar and rebuild the event list.
  bool refresh() {
    if (!wifiConnected() && !connectWiFi()) return false;
    if (!_cfg.isCalendarConfigured()) {
      _last_error = "calendar url not configured";
      return false;
    }
    if (!_time_synced) syncTime();

    Serial.printf("[Calendar] fetching %s\n", _cfg.ics_url.c_str());

    HTTPClient http;
    WiFiClientSecure secure_client;
    bool https = _cfg.ics_url.startsWith("https://");
    if (https) {
      secure_client.setInsecure();  // ICS feeds rarely need cert pinning here.
      if (!http.begin(secure_client, _cfg.ics_url)) {
        _last_error = "http begin failed";
        return false;
      }
    } else {
      if (!http.begin(_cfg.ics_url)) {
        _last_error = "http begin failed";
        return false;
      }
    }
    http.setTimeout(15000);
    http.addHeader("Accept", "text/calendar");

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
      _last_error = "http status " + String(code);
      Serial.printf("[Calendar] HTTP error: %d\n", code);
      http.end();
      return false;
    }

    std::vector<CalendarEvent> parsed;
    parseHttp(http, parsed);
    http.end();

    finalize(parsed);
    _events.swap(parsed);
    _last_refresh = time(nullptr);
    _last_error = "";
    Serial.printf("[Calendar] refresh done, %d upcoming event(s).\n", (int)_events.size());
    return true;
  }

  // Call frequently from loop(): fires reminders that have become due.
  void update() {
    if (!_time_synced) return;
    time_t now = time(nullptr);
    for (auto &ev : _events) {
      if (ev.reminder_fired) continue;
      // Fire once we reach the reminder time, but not for events long past.
      if (now >= ev.reminder_epoch && now <= ev.start_epoch + 60) {
        ev.reminder_fired = true;
        if (_cb) _cb(ev);
      }
    }
  }

  // Next upcoming (not yet started) event, or nullptr.
  const CalendarEvent *nextEvent() const {
    time_t now = time(nullptr);
    const CalendarEvent *best = nullptr;
    for (const auto &ev : _events) {
      if (ev.start_epoch >= now && (best == nullptr || ev.start_epoch < best->start_epoch)) {
        best = &ev;
      }
    }
    return best;
  }

private:
  CalendarConfig _cfg;
  ReminderCallback _cb = nullptr;
  std::vector<CalendarEvent> _events;
  bool _time_synced = false;
  String _last_error = "";
  time_t _last_refresh = 0;

  // --- ICS parsing ---------------------------------------------------------

  void parseHttp(HTTPClient &http, std::vector<CalendarEvent> &out) {
    WiFiClient *stream = http.getStreamPtr();
    if (stream == nullptr) return;

    int remaining = http.getSize();  // -1 when the length is unknown (chunked)

    String logical = "";  // current unfolded logical line
    String raw = "";      // bytes of the physical line being read
    bool in_event = false;
    CalendarEvent current;

    unsigned long last_data = millis();
    const unsigned long idle_timeout = 8000;  // stop if the feed stalls

    while (remaining != 0 && (http.connected() || stream->available() > 0)) {
      size_t avail = stream->available();
      if (avail == 0) {
        if (millis() - last_data > idle_timeout) break;
        delay(1);
        continue;
      }
      last_data = millis();

      while (avail-- > 0) {
        int c = stream->read();
        if (c < 0) break;
        if (remaining > 0) remaining--;

        if (c == '\r') continue;
        if (c == '\n') {
          if (raw.length() > 0 && (raw[0] == ' ' || raw[0] == '\t')) {
            logical += raw.substring(1);  // RFC 5545 folded continuation
          } else {
            handleLine(logical, in_event, current, out);
            logical = raw;
          }
          raw = "";
        } else {
          raw += (char)c;
          if (raw.length() > 1024) raw.remove(0, raw.length() - 1024);
        }
      }
    }

    // Flush whatever remains.
    if (raw.length() > 0) {
      if (raw[0] == ' ' || raw[0] == '\t') logical += raw.substring(1);
      else { handleLine(logical, in_event, current, out); logical = raw; }
    }
    handleLine(logical, in_event, current, out);
  }

  void handleLine(const String &line, bool &in_event, CalendarEvent &current,
                  std::vector<CalendarEvent> &out) {
    if (line.length() == 0) return;

    if (line == "BEGIN:VEVENT") {
      in_event = true;
      current = CalendarEvent();
      return;
    }
    if (line == "END:VEVENT") {
      if (in_event && current.start_epoch > 0) {
        if (current.title.length() == 0) current.title = "(no title)";
        out.push_back(current);
      }
      in_event = false;
      return;
    }
    if (!in_event) return;

    // Split "NAME[;params]:VALUE"
    int colon = line.indexOf(':');
    if (colon < 0) return;
    String name = line.substring(0, colon);
    String value = line.substring(colon + 1);

    String prop = name;
    int semi = name.indexOf(';');
    if (semi >= 0) prop = name.substring(0, semi);
    prop.toUpperCase();

    if (prop == "SUMMARY") {
      value = unescapeText(value);
      if (value.length() > 60) value = value.substring(0, 60);
      current.title = value;
    } else if (prop == "DTSTART") {
      current.start_epoch = parseDateTime(value);
    }
  }

  static String unescapeText(String v) {
    v.replace("\\n", " ");
    v.replace("\\N", " ");
    v.replace("\\,", ",");
    v.replace("\\;", ";");
    v.replace("\\\\", "\\");
    return v;
  }

  // Convert an ICS date/time value to a UTC epoch. Handles:
  //   20260808T130000Z   (UTC)
  //   20260808T130000    (floating / local, TZID)
  //   20260808           (all-day date)
  time_t parseDateTime(const String &raw) {
    String v = raw;
    v.trim();
    if (v.length() < 8) return 0;

    bool is_utc = v.endsWith("Z");

    int y = v.substring(0, 4).toInt();
    int mo = v.substring(4, 6).toInt();
    int d = v.substring(6, 8).toInt();
    int h = 0, mi = 0, s = 0;
    int tpos = v.indexOf('T');
    if (tpos == 8 && v.length() >= 15) {
      h = v.substring(9, 11).toInt();
      mi = v.substring(11, 13).toInt();
      s = v.substring(13, 15).toInt();
    }
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;

    time_t utc = epochFromUTC(y, mo, d, h, mi, s);
    if (!is_utc) {
      // Wall-clock time in the configured local zone -> UTC.
      utc -= (_cfg.gmt_offset_sec + _cfg.daylight_offset_sec);
    }
    return utc;
  }

  // Days-from-civil algorithm (Howard Hinnant) -> UTC epoch, no libc TZ needed.
  static time_t epochFromUTC(int y, int m, int d, int h, int mi, int s) {
    int yy = y - (m <= 2 ? 1 : 0);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = (long long)era * 146097 + (long long)doe - 719468;
    return (time_t)(days * 86400LL + h * 3600LL + mi * 60LL + s);
  }

  // Drop past events, sort ascending, compute reminder times, cap the count.
  void finalize(std::vector<CalendarEvent> &list) {
    time_t now = _time_synced ? time(nullptr) : 0;
    std::vector<CalendarEvent> keep;
    for (auto &ev : list) {
      // Keep events that have not started yet (with a small grace window).
      if (now == 0 || ev.start_epoch + 60 >= now) {
        ev.reminder_epoch = ev.start_epoch - (time_t)_cfg.reminder_lead_sec;
        // If the reminder time already passed but the event is imminent,
        // fire it right away by leaving reminder_epoch in the past.
        keep.push_back(ev);
      }
    }
    std::sort(keep.begin(), keep.end(),
              [](const CalendarEvent &a, const CalendarEvent &b) {
                return a.start_epoch < b.start_epoch;
              });
    if (keep.size() > _cfg.max_events) keep.resize(_cfg.max_events);
    list.swap(keep);
  }
};

#endif  // CALENDAR_CLIENT_HPP_
