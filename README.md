# stilhawt-esp32-kit

**English** · [Français](README.fr.md)

**A small ESP32 base and two firmwares built on it**: a bathroom scale read over Bluetooth, and a
pulse water meter that tells real flow from ghost pulses.

Everything builds with [PlatformIO](https://platformio.org). No online service is required: the
devices publish over MQTT to **your** broker, and each one serves a small local web page.

## The base (`lib/`)

| Header | What it brings |
|---|---|
| `StilhawtWifi.h` | non-blocking WiFi, several remembered networks, a fallback **captive portal** when none answers, a status LED |
| `StilhawtOta.h` | updates **over the network** (OTA) — **without a password** today (see "Security") |
| `StilhawtWeb.h` | a local web page: the device's identity, `/state` as JSON, the network list — and forms that **change** the configuration (WiFi, MQTT, calibration), **without authentication** |
| `mqtt_ca.h` | a slot for your broker's certificate authority — **not read yet** by the firmwares (see "Security") |

## The two examples

**`examples/water-meter`** — TTGO T-Display, a water meter with a **dual output, three wires**:
black = common (GND), red = OUT1 on **GPIO27** (1 pulse = 1 litre — this is what counts),
green = OUT2 on **GPIO25** (a second output whose role is still to be characterised: flow direction,
inverted contacts or tamper detection — it is logged and does not affect the count).
- Counts the day and the last 30 days, keeps the totals in non-volatile memory, syncs on NTP.
- **Rejects impossible pulses**: even at 60 L/min, two litres are at least one second apart. A gap
  under 700 ms is not water but electrical noise (a loose ground wire is enough). These pulses are
  refused **and counted** (`glitches` in the published state), so a night-time "micro-leak" reads
  for what it is.
- Two screens, a button, the local web page, MQTT.

**`examples/etekcity-scale`** — ESP32 (WROOM), headless: a Bluetooth → MQTT bridge for an Etekcity
FIT-8S impedance scale.
- The scale **broadcasts** its weight and impedance in its Bluetooth advertisements; the ESP32 listens
  passively, without pairing, and publishes them. No body-composition computation here: raw values
  only.
- The advertisement format is described at the top of `src/main.cpp`.

## Getting started

You need Python 3 and `git`.

```bash
git clone https://github.com/StilHawt/stilhawt-esp32-kit
cd stilhawt-esp32-kit
pip install platformio
```

Then, for each example:

1. **Configure** `examples/<example>/src/zdt_device.h`: the device's name, the base of its MQTT
   topics, your broker's address and port. TLS is off by default. `ZDT_MQTT_TLS 1` (and port 8883)
   **encrypts** the connection, but the broker is **not verified** (see "Security"). The value must be
   exactly `0` or `1`: any other value compiles and turns TLS off.
2. **Build**: `pio run -d examples/water-meter` (or `examples/etekcity-scale`).
3. **Flash** the board plugged in over USB: `pio run -d examples/water-meter -t upload`.
4. **Give it WiFi and the MQTT account** over the serial port (115200 baud), never in the code:
   `WIFI <ssid> <password>` then `MQTT <user> <password>`. With no known network, the device opens
   its captive portal. **The broker must have an account**: without one the firmware never tries to
   connect, so an anonymous broker cannot be used today.

Later updates can go over the network (OTA): `pio run -d examples/water-meter -t upload
--upload-port <device address>`. **The scale** is the exception: Bluetooth and WiFi share the radio,
so its OTA only opens for 120 seconds after it receives `{"ota":true}` on its MQTT `cmd` topic (it
restarts to open it) — a working broker is needed.

## Security — read before plugging in

These firmwares are made for a **trusted network** (your home network), not an open one. Three known
limits, stated as they are:

- **TLS without server verification.** With `ZDT_MQTT_TLS 1` the connection is encrypted, but the
  firmware calls `setInsecure()`: it does not verify the broker's identity, and `lib/mqtt_ca.h` is not
  read. Someone on the path could impersonate your broker.
- **OTA without a password.** Any machine on the same network can reflash the device (port 3232).
- **Web page without authentication.** The local page's forms change WiFi, the MQTT account or the
  calibration, without a password.

These three points are the next work items of this repository.

## Status

Both examples are **built** at every version, from this repository alone, in a fresh container. The
**water meter** was also **run in the Wokwi simulator** from this repository (a public test broker,
TLS off): WiFi given over serial, 3 real litres and 2 impossible pulses → `total_l=3`, `glitches=2`
in the published state. Not simulated: the scale (Bluetooth), the screen, OTA and the web page. Not yet
replayed on a physical board from this repository. Code comments are in French.

## License

Apache License 2.0 — see [LICENSE](LICENSE). Copyright 2026 StilHawt.
