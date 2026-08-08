/*
  ControlAPI.hpp

  A small HTTP REST control API for Stack-chan. It lets you query and refresh
  the remote calendar, list the upcoming events, change the speaker volume,
  trigger a test reminder and make the avatar speak.

  Endpoints
    GET  /                  human readable help
    GET  /status            device + calendar status (JSON)
    GET  /events            upcoming events (JSON)
    POST /calendar/refresh  re-download the remote calendar
    POST /volume            set volume  (?value=0..255 or JSON {"volume":N})
    POST /speak             show text    (JSON {"text":"..","expression":0..6})
    POST /reminder/test     fire a test reminder chime

  When api.auth_token is set in SC_CalendarConfig.yaml every request must send
    Authorization: Bearer <token>
*/
#ifndef CONTROL_API_HPP_
#define CONTROL_API_HPP_

#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <functional>

#include "CalendarClient.hpp"

class ControlAPI {
public:
  // Hooks the main sketch provides so the API can act on the device.
  std::function<void(uint8_t)> setVolume = nullptr;
  std::function<uint8_t()> getVolume = nullptr;
  std::function<void(const String &, int)> speak = nullptr;  // (text, expression)
  std::function<void()> testReminder = nullptr;
  std::function<bool()> refreshCalendar = nullptr;
  std::function<bool()> bluetoothMode = nullptr;

  ControlAPI() {}

  void begin(CalendarClient *cal, uint16_t port, const String &auth_token) {
    _cal = cal;
    _auth_token = auth_token;

    if (_server != nullptr) {
      _server->stop();
      delete _server;
    }
    _server = new WebServer(port);

    _server->on("/", HTTP_GET, [this]() { handleRoot(); });
    _server->on("/status", HTTP_GET, [this]() { handleStatus(); });
    _server->on("/events", HTTP_GET, [this]() { handleEvents(); });
    _server->on("/calendar/refresh", HTTP_POST, [this]() { handleRefresh(); });
    _server->on("/volume", HTTP_POST, [this]() { handleVolume(); });
    _server->on("/speak", HTTP_POST, [this]() { handleSpeak(); });
    _server->on("/reminder/test", HTTP_POST, [this]() { handleTestReminder(); });
    _server->onNotFound([this]() { _server->send(404, "application/json", "{\"error\":\"not found\"}"); });

    // WebServer only keeps headers we explicitly ask for.
    static const char *header_keys[] = {"Authorization"};
    _server->collectHeaders(header_keys, 1);

    _server->begin();
    _running = true;
    Serial.printf("[API] control API listening on port %u\n", port);
  }

  void stop() {
    if (_running && _server != nullptr) {
      _server->stop();
      _running = false;
    }
  }

  bool running() const { return _running; }

  void handleClient() {
    if (_running && _server != nullptr) _server->handleClient();
  }

private:
  WebServer *_server = nullptr;
  CalendarClient *_cal = nullptr;
  String _auth_token = "";
  bool _running = false;

  bool authorized() {
    if (_auth_token.length() == 0) return true;
    if (!_server->hasHeader("Authorization")) return false;
    String expected = "Bearer " + _auth_token;
    return _server->header("Authorization") == expected;
  }

  bool guard() {
    if (authorized()) return true;
    _server->send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return false;
  }

  static String isoLocal(time_t epoch) {
    if (epoch <= 0) return "";
    struct tm tm_info;
    localtime_r(&epoch, &tm_info);
    char buf[25];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return String(buf);
  }

  void handleRoot() {
    String body =
      "Stack-chan Calendar Reminder - Control API\n\n"
      "GET  /status            device + calendar status\n"
      "GET  /events            upcoming events\n"
      "POST /calendar/refresh  re-download the remote calendar\n"
      "POST /volume            ?value=0..255 or {\"volume\":N}\n"
      "POST /speak             {\"text\":\"..\",\"expression\":0..6}\n"
      "POST /reminder/test     fire a test reminder\n";
    _server->send(200, "text/plain", body);
  }

  void handleStatus() {
    if (!guard()) return;
    StaticJsonDocument<512> doc;
    doc["wifi_connected"] = _cal && _cal->wifiConnected();
    doc["ip"] = WiFi.localIP().toString();
    doc["time_synced"] = _cal && _cal->timeSynced();
    doc["now"] = isoLocal(time(nullptr));
    if (_cal) {
      doc["last_refresh"] = isoLocal(_cal->lastRefresh());
      doc["event_count"] = (int)_cal->events().size();
      doc["last_error"] = _cal->lastError();
      const CalendarEvent *next = _cal->nextEvent();
      if (next) {
        JsonObject n = doc.createNestedObject("next_event");
        n["title"] = next->title;
        n["start"] = isoLocal(next->start_epoch);
      }
    }
    if (getVolume) doc["volume"] = getVolume();
    if (bluetoothMode) doc["bluetooth_mode"] = bluetoothMode();

    String out;
    serializeJson(doc, out);
    _server->send(200, "application/json", out);
  }

  void handleEvents() {
    if (!guard()) return;
    if (!_cal) {
      _server->send(200, "application/json", "[]");
      return;
    }
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();
    for (const auto &ev : _cal->events()) {
      JsonObject o = arr.createNestedObject();
      o["title"] = ev.title;
      o["start"] = isoLocal(ev.start_epoch);
      o["reminder"] = isoLocal(ev.reminder_epoch);
      o["fired"] = ev.reminder_fired;
    }
    String out;
    serializeJson(doc, out);
    _server->send(200, "application/json", out);
  }

  void handleRefresh() {
    if (!guard()) return;
    bool ok = refreshCalendar ? refreshCalendar() : false;
    int count = _cal ? (int)_cal->events().size() : 0;
    String msg = _cal ? _cal->lastError() : String("no calendar");
    String out = String("{\"ok\":") + (ok ? "true" : "false") +
                 ",\"event_count\":" + count +
                 ",\"error\":\"" + (ok ? "" : msg) + "\"}";
    _server->send(ok ? 200 : 502, "application/json", out);
  }

  void handleVolume() {
    if (!guard()) return;
    int vol = -1;
    if (_server->hasArg("value")) {
      vol = _server->arg("value").toInt();
    } else if (_server->hasArg("plain")) {
      StaticJsonDocument<128> doc;
      if (deserializeJson(doc, _server->arg("plain")) == DeserializationError::Ok) {
        if (doc.containsKey("volume")) vol = doc["volume"].as<int>();
      }
    }
    if (vol < 0 || vol > 255) {
      _server->send(400, "application/json", "{\"error\":\"volume must be 0..255\"}");
      return;
    }
    if (setVolume) setVolume((uint8_t)vol);
    _server->send(200, "application/json", String("{\"ok\":true,\"volume\":") + vol + "}");
  }

  void handleSpeak() {
    if (!guard()) return;
    if (!_server->hasArg("plain")) {
      _server->send(400, "application/json", "{\"error\":\"missing json body\"}");
      return;
    }
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, _server->arg("plain")) != DeserializationError::Ok) {
      _server->send(400, "application/json", "{\"error\":\"invalid json\"}");
      return;
    }
    String text = doc["text"] | "";
    int expression = doc["expression"] | 5;  // 5 = Neutral
    if (speak) speak(text, expression);
    _server->send(200, "application/json", "{\"ok\":true}");
  }

  void handleTestReminder() {
    if (!guard()) return;
    if (testReminder) testReminder();
    _server->send(200, "application/json", "{\"ok\":true}");
  }
};

#endif  // CONTROL_API_HPP_
