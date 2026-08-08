# stackchan-calendar-reminder

Stack-chan as a **desktop calendar reminder** for **M5Stack Core Gray (Core1)**.

# Overview

The device connects to Wi-Fi, keeps the time in sync via NTP, downloads a
**remote calendar** in iCalendar (`.ics`) format and announces upcoming events
with **audio (a chime or a WAV file), an LED flash and a speech balloon** on the
avatar. A **Wi-Fi status indicator** is shown in the top-left corner of the
avatar screen, the current **connection status / errors** are shown in the
speech balloon, and a built-in **HTTP control API** lets you query/refresh the
calendar, change the volume, trigger a test reminder, make Stack-chan speak and
update the firmware over the air.

**The servo control and the Bluetooth speaker (A2DP) audio code** that the
original sketch shipped with **have both been removed** – this build drives no
servo and is not a Bluetooth speaker.

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
- [stackchan-arduino](https://github.com/mongonta0716/stackchan-arduino)
- [ArduinoJson](https://arduinojson.org/) 7.x (control API responses)
- [FastLED](https://github.com/FastLED/FastLED) (GoBottom LEDs)
- WiFi / HTTPClient / WebServer / Update (bundled with the ESP32 Arduino core)

See [platformio.ini](platformio.ini) for exact versions.

# Configuration

A **single** YAML file holds every firmware setting. Copy it to the SD card as
`/yaml/SC_Config.yaml` (see [data/yaml/SC_Config.yaml](data/yaml/SC_Config.yaml)).
It is read by both the stackchan-arduino config loader (speaker volume, balloon
font, LEDs) and by this firmware (Wi-Fi, calendar, reminder, control API). If
the file is missing, defaults are used and the calendar feature stays disabled
until at least the Wi-Fi SSID and the calendar URL are provided.

The calendar-related part of the file looks like this:

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

# On-screen status

- **Wi-Fi indicator** (top-left corner): green signal bars when connected (the
  number of lit bars reflects the RSSI: 4 bars ≥ −55 dBm, 3 ≥ −65, 2 ≥ −75,
  1 otherwise), or red bars with a slash when not connected.
- **Status balloon**: shows the current state / error, e.g. `WiFi disconnected`,
  `Time not synced`, `Cal error: <reason>`, or `Next: <event title>` /
  `Calendar OK (N)` when everything is healthy. A reminder or a manual refresh
  temporarily takes over the balloon.

# Control API

When `api.enabled` is `true` the device serves a small REST API on the
configured port. If `auth_token` is set, every request must send
`Authorization: Bearer <token>`.

| Method & path | Description |
|---------------|-------------|
| `GET  /` | Human-readable help. |
| `GET  /status` | Wi-Fi/time/calendar status, RSSI, volume, next event (JSON). |
| `GET  /events` | Upcoming events with start/reminder times (JSON). |
| `POST /calendar/refresh` | Re-download the remote calendar now. |
| `POST /volume` | Set volume: `?value=0..255` or body `{"volume":N}`. |
| `POST /speak` | Body `{"text":"..","expression":0..6}` – show text on the avatar. |
| `POST /reminder/test` | Fire a test reminder (chime + balloon + LED). |
| `GET  /update` | OTA firmware update web page (upload a `.bin`). |
| `POST /update` | OTA firmware upload (`multipart/form-data`). |

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

# OTA firmware update (via web page)

The firmware can update itself over Wi-Fi — no USB cable needed after the first
flash. Open:

```
http://<device-ip>/update
```

Pick a firmware `.bin` and press **Upload**; a progress bar is shown and the
device reboots automatically when the update finishes. If `api.auth_token` is
set, append it as a query parameter: `http://<device-ip>/update?token=<token>`
(a plain browser upload form cannot send the `Authorization` header).

Which `.bin` to upload: the application image built by PlatformIO, i.e.
`.pio/build/m5stack-grey/firmware.bin` (also attached to each GitHub Release).
The 16MB partition layout (`default_16MB.csv`) keeps two OTA app slots, so the
running firmware is preserved until the new image is verified.

You can also push builds straight from PlatformIO over the network with
`espota` — see the commented `upload_protocol`/`upload_port` lines in
[platformio.ini](platformio.ini).

# Building the firmware

```sh
pio run -e m5stack-grey                 # build  -> .pio/build/m5stack-grey/firmware.bin
pio run -e m5stack-grey -t upload       # flash over USB (first time)
pio run -e m5stack-grey -t uploadfs     # upload the /data (YAML) filesystem image
```

CI (`.github/workflows/build.yml`) builds the firmware on every push and
uploads it as an artifact. Pushing a `v*` tag (or running the workflow manually
with a `tag` input) publishes a **GitHub Release** with `firmware.bin`,
`bootloader.bin` and `partitions.bin` attached.

# Usage (buttons)

- **BtnA**: refresh the remote calendar now.
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
