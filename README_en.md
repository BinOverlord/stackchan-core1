# stackchan-calendar-reminder

[Japanese](README.md) | English

# Overview

Stack-chan as a **desktop calendar reminder** for **M5Stack Core Gray (Core1)**.

The device connects to Wi-Fi, keeps the time in sync via NTP, downloads a
**remote calendar** in iCalendar (`.ics`) format and announces upcoming events
with **audio (a chime or a WAV file), an LED flash and a speech balloon** on the
avatar. A built-in **HTTP control API** lets you query/refresh the calendar,
change the volume, trigger a test reminder and make Stack-chan speak.

The original Bluetooth speaker mode is kept as a secondary mode. **The servo
control that the original sketch shipped with has been removed** – this build
does not drive any servo.

Based on the `Bluetooth_with_ESP32A2DP` example from
[M5Unified](https://github.com/m5stack/M5Unified).

# Development Environment
- VSCode
- PlatformIO

# Supported Model

- **M5Stack Core Gray (Core1)** – primary target (`env:m5stack-grey`).
  M5Stack Basic (16MB Flash) also works with the same env.

Other envs (Core2 / Fire / Core-ESP32) are still present in `platformio.ini`
and will build, but the calendar reminder + control API feature is developed
and tuned for the Core Gray.

# Required Libraries

- [M5Stack-Avatar](https://github.com/meganetaaan/m5stack-avatar)
- [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio)
- [stackchan-arduino](https://github.com/mongonta0716/stackchan-arduino)
- [ArduinoJson](https://arduinojson.org/) (control API responses)
- WiFi / HTTPClient / WebServer (bundled with the ESP32 Arduino core)

See [platformio.ini](platformio.ini) for exact versions.

# Configuration

Two YAML files are used, both placed under `/yaml/` on the SD card:

| File | Purpose |
|------|---------|
| `SC_BasicConfig.yaml` | Avatar / speaker / LED / Bluetooth settings (kept from the original app). |
| `SC_CalendarConfig.yaml` | **New** – Wi-Fi, remote calendar URL, NTP/timezone, reminder and control-API settings. |

If a file is missing, sensible defaults are used (and the calendar feature stays
disabled until at least the Wi-Fi SSID and the calendar URL are provided).

## `SC_CalendarConfig.yaml`

```yaml
wifi:
  ssid: "your-wifi-ssid"
  password: "your-wifi-password"
  connect_timeout_sec: 20

calendar:
  ics_url: "https://example.com/basic.ics"  # any public/secret .ics feed
  poll_interval_sec: 300                     # how often to re-download
  reminder_lead_sec: 300                     # fire this long before the event
  max_events: 20

time:
  ntp_server: "pool.ntp.org"
  gmt_offset_sec: -21600                      # default: Mexico City (UTC-6)
  daylight_offset_sec: 0

reminder:
  tone_enabled: true
  speak_enabled: true
  chime_repeat: 2
  sound_file: ""                              # optional WAV on SD, e.g. /sound/reminder.wav
  led_flash: true

api:
  enabled: true
  port: 80
  auth_token: ""                              # optional Bearer token
```

The `ics_url` works with any iCalendar feed, for example Google Calendar's
"Secret address in iCal format", or a published Outlook / Nextcloud / iCloud
calendar. Both `http://` and `https://` URLs are supported (TLS certificates are
not verified).

> Note: recurring events (`RRULE`) are read as their single `DTSTART`
> occurrence; per-occurrence expansion is not performed on the device.

# Control API

When `api.enabled` is `true` the device serves a small REST API on the
configured port. If `auth_token` is set, every request must send
`Authorization: Bearer <token>`.

| Method & path | Description |
|---------------|-------------|
| `GET  /` | Human-readable help. |
| `GET  /status` | Wi-Fi/time/calendar status, volume, next event (JSON). |
| `GET  /events` | Upcoming events with start/reminder times (JSON). |
| `POST /calendar/refresh` | Re-download the remote calendar now. |
| `POST /volume` | Set volume: `?value=0..255` or body `{"volume":N}`. |
| `POST /speak` | Body `{"text":"..","expression":0..6}` – show text on the avatar. |
| `POST /reminder/test` | Fire a test reminder (chime + balloon + LED). |

Examples:

```sh
curl http://<device-ip>/status
curl -X POST http://<device-ip>/calendar/refresh
curl -X POST "http://<device-ip>/volume?value=120"
curl -X POST http://<device-ip>/speak -d '{"text":"Hello!","expression":0}'
curl -X POST http://<device-ip>/reminder/test
```

Avatar expression values: `0` Happy, `1` Angry, `2` Sad, `3` Doubt, `4` Sleepy,
`5` Neutral.

# Usage (buttons)

- **BtnA – single click**: switch to **Bluetooth speaker** mode (Wi-Fi and the
  calendar/API are stopped, because Bluetooth Classic and Wi-Fi share the radio).
- **BtnA – double click**: switch back to **Calendar** mode (Wi-Fi reconnects,
  the calendar is refreshed and the control API restarts).
- **BtnB**: decrease volume.
- **BtnC**: increase volume.

# Credit
- [meganetaaan](https://github.com/meganetaaan)
- [lovyan03](https://github.com/lovyan03/LovyanGFX)
- [robo8080](https://github.com/robo8080)
- [tobozo](https://github.com/tobozo)

# LICENSE
[MIT](LICENSE)

# Author
[Takao Akaki](https://github.com/mongonta0716)
