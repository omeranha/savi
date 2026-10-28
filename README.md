# SAVI

Bracelet nodes and a Wi-Fi gateway collect vitals over **ESP-NOW** and publish them over **MQTT** to a Python middleware service, which writes telemetry and alerts to Supabase.

```
MAX30102 / MAX30205 / BMI270
        │ I2C
   ESP32-C6 bracelet  ──ESP-NOW──►  ESP32 gateway  ──MQTT──►  Python middleware ──► Supabase
   LED + buzzer                     (STA on PC hotspot)
```

| Folder | Role | Intended chip |
| --- | --- | --- |
| `esp32-c6/` | Wearable node: sensors, battery, events, ESP-NOW TX | ESP32-C6 |
| `esp32-gateway/` | Relays bracelet traffic to MQTT and reports its own state | ESP32 (or other STA-capable ESP) |
| `mqtt_middleware/` | Local MQTT broker and Supabase REST writer | PC running Python |
| `components/savi_proto/` | The ESP-NOW packet layout both firmwares share | header only |
| `components/savi_battery/` | ADC battery gauge both firmwares share | — |

Requires [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c6/get-started/index.html). GPIO and I2C use `esp_driver_gpio` and `esp_driver_i2c` (not the old `driver` component name).

---

## Bracelet (`esp32-c6`)

Ships with `SAVI_DEMO_MODE` on, which generates plausible vitals instead of
touching I2C. The bracelet reads its sensors and battery, drives an LED and buzzer
on events, asks a gateway for an `esp_id`, and streams telemetry.

| Function | Part | Notes |
| --- | --- | --- |
| Heart rate / SpO2 | MAX30102 (`0x57`) | SpO2 mode, 50 Hz, red + IR |
| Skin temperature | MAX30205 (`0x48`) | One-shot about every 250 ms |
| Accel / gyro | BMI270 (`0x68`) | 50 Hz, advanced power save after config |
| Motion LED | GPIO 4 | 200 ms blink while any event is active |
| Buzzer | GPIO 5 | Held high while any event is active |
| Battery gauge | ADC GPIO (default 3) | On by default, see below |

Default I2C: **SDA GPIO 6**, **SCL GPIO 7**, 400 kHz. Change pins in `esp32-c6/main/sensors.h`.

### Runtime

1. Broadcast `MSG_HELLO`. Every gateway in range answers `MSG_WELCOME`; take the one with the strongest signal and cache its MAC and channel in NVS.
2. If `esp_id` is 0, send `MSG_NEED_ID` to that gateway and wait for `MSG_ASSIGN_ID`. The ID is cached in NVS (`savi` / `esp_id`), so this happens once in the bracelet's life.
3. Every **50 ms**: read the sensors (or demo values), read the battery, drive the LED and buzzer, and fire events on their rising edges.
4. Every **1 s** (once it has an ID and a gateway): send a packed `MSG_SENSOR_DATA` frame.
5. After **10** unacked sends in a row, drop that gateway and go back to step 1.

Events: temperature ≥ **38 °C** fires `EVENT_TEMP_HIGH`, movement fires `EVENT_MOTION`,
and charge at or below `SAVI_BATTERY_LOW_PCT` (default 15%) fires `EVENT_BATTERY_LOW`.
Each fires once per crossing and rearms only after the condition clears. **Any**
active event lights the LED and sounds the buzzer.

Identity is split in two on purpose. The MAC is what the gateways and the
middleware key on, so a bracelet keeps its row whichever gateway relays it and a
reflash changes nothing. `esp_id` is the small integer the database generates and
the dashboard searches on, handed to the bracelet on request. Discovery never
waits on the middleware, so failover stays fast even if the PC is down.

Radio: Wi-Fi **PS none** while probing or waiting for an ID; then **min modem sleep**.

### Demo mode

`SAVI_DEMO_MODE` (on by default) is the bool that swaps the I2C sensors for
plausible synthetic vitals, including a battery that drains 1% every 6 seconds so
the `BATTERY_LOW` path can be exercised on a bare board. Turn it off in
`idf.py menuconfig` → *SAVI Bracelet Configuration* to drive the real hardware.

### Battery gauge

Both the bracelet and the gateway read their cell through a resistor divider,
using the shared [`components/savi_battery`](./components/savi_battery):

```
VBAT --[ R_high ]--+--[ R_low ]-- GND
                   │
                ADC GPIO
```

Configure it in `idf.py menuconfig` → *Component config* → *SAVI battery gauge*:
sense GPIO (default **3** on the C6, **34** on the ESP32 gateway), both resistors
in kΩ, and the mV treated as 0% and 100% (3300/4200 by default). It is on by
default, so turn `SAVI_BATTERY_MONITOR` off on a board with no divider wired —
otherwise the pin floats and the reading is meaningless. While off, the device
reports `SAVI_BATTERY_PCT_UNKNOWN` and the battery column keeps its last value.

The percentage is a straight line between the two voltages. Replace
`percent_from_mv()` once you characterise the cell: a 1S li-ion sits near 3.7 V
for most of its capacity, so a line reads optimistic in the middle.

### Failover between gateways

Any gateway can serve any bracelet — they share the ESP-NOW keys and the bracelet
picks one at runtime, so adding a second gateway needs no bracelet reflash.

A bracelet notices its gateway is gone through ESP-NOW acks, not timeouts: ten
unacked sends (about **10 s** at one report per second) drop it. The bracelet then
probes its current channel, and if nothing answers it sweeps channels 1–13 at
600 ms each, so a gateway on another channel is found within about **8 s** more.
A full sweep that finds nothing backs off from 2 s up to 30 s before sweeping
again, so a bracelet out of range of everything does not drain the cell probing.

Selection is sticky: the gateway in use wins any re-probe regardless of signal,
and the bracelet only switches when that one stops answering. Two gateways
covering the same room therefore cannot make a bracelet flap between them. The
trade-off is that a bracelet does not migrate to a closer gateway while its
current one still works.

Failover needs nothing on the server side. Telemetry carries the bracelet MAC, so
whichever gateway relays it the middleware upserts the same `braceletes` row.

A gateway keeps no per-bracelet state beyond a 16-slot table of who was heard
recently. ESP-NOW delivers plaintext unicast without the sender being a peer, so
its peer table holds exactly one entry — the broadcast address it needs to answer
probes — and there is no limit on how many bracelets it can serve beyond the
16-deep receive queue.

Dropping the hardcoded gateway MAC does mean a bracelet trusts whatever answers
its probe. Nothing on this hop is authenticated or encrypted (see below), so
treat the ESP-NOW side as an open radio link.

Set the initial **ESP-NOW channel** in `idf.py menuconfig` → *SAVI Bracelet Configuration* to the **access-point channel the gateway joined** (the gateway logs that channel at boot). It only matters on the first boot: the bracelet sweeps channels until the gateway answers and then caches the channel it reports in NVS.

```bash
cd esp32-c6
idf.py set-target esp32c6
idf.py menuconfig   # SAVI Bracelet Configuration → initial channel, sensors, battery gauge
idf.py -p PORT flash monitor
```

---

## Gateway (`esp32-gateway`)

Connects as a Wi-Fi station (modem sleep off so ESP-NOW RX stays up) and publishes
to the configured MQTT broker. The Python middleware on the PC runs the broker and
writes messages to Supabase. At startup the gateway
synchronizes its clock with `pool.ntp.org` and uses UTC−3; startup waits for a
successful sync before enabling ESP-NOW processing. Telemetry timestamps use
ISO-8601 with an explicit `-03:00` offset.

| Incoming | Action |
| --- | --- |
| `MSG_HELLO` | Answer `MSG_WELCOME` with the live channel |
| `MSG_NEED_ID` | Ask the middleware over MQTT, then broadcast `MSG_ASSIGN_ID` |
| `MSG_SENSOR_DATA` | Publish the vitals plus the bracelet MAC to `savi/telemetry` |
| `MSG_SENSOR_EVENT` | Publish the alert to `savi/alert` with `tipo` set to the event name |

Every minute the gateway also publishes its own row to `savi/gateway`: where it
is, its battery percentage, and how many bracelets are streaming to it. A bracelet
counts as connected to this gateway when it has unicast something in the last
**90 s** — a `HELLO` does not count, since that is broadcast to every gateway in
range and proves nothing about which one it chose.

The gateway keeps no per-bracelet state beyond that 16-slot seen-recently table.
Run as many gateways as you like: they need no coordination and no per-bracelet
configuration. Only ID allocation involves the middleware, and it runs on its own
task so a slow or missing broker never blocks the packet path.

Configure the Wi-Fi SSID/password, the MQTT broker URI/username/password and
*Where this gateway is installed* (written to `gateways.local`) in
`idf.py menuconfig` → *Gateway Configuration*. Use `mqtts://` for a TLS
connection; the gateway verifies broker certificates using the ESP certificate
bundle.

The gateway publishes with QoS 1 and subscribes only to `savi/id/response`:

| Topic | Direction | JSON payload |
| --- | --- | --- |
| `savi/telemetry` | Gateway → broker | `{"mac":"aa:bb:cc:dd:ee:ff","esp_id":1,"atualizado_em":"...-03:00","temperatura":36.5,"bpm":72,"spo2":98,"acc_x":0.1,"acc_y":0.2,"acc_z":0.3,"gyro_x":1.0,"gyro_y":2.0,"gyro_z":3.0,"bateria":64}` |
| `savi/alert` | Gateway → broker | `{"mac":"aa:bb:cc:dd:ee:ff","esp_id":1,"tipo":"BATTERY_LOW","valor":12}` |
| `savi/gateway` | Gateway → broker | `{"mac":"11:22:33:44:55:66","local":"Ward 2","braceletes":3,"atualizado_em":"...-03:00","bateria":88}` |
| `savi/gateway/offline` | Broker → middleware | `{"mac":"11:22:33:44:55:66"}`, the gateway's last will |
| `savi/id/request` | Gateway → broker | `{"mac":"aa:bb:cc:dd:ee:ff"}` |
| `savi/id/response` | Broker → gateway | `{"mac":"aa:bb:cc:dd:ee:ff","esp_id":1}` |

`bateria` appear only when that device has a working
gauge. `atualizado_em` is left out until NTP lands, and PostgreSQL stamps the row
instead — a reading is never dropped for want of a clock.

`savi/gateway/offline` is an MQTT last will: the gateway registers it at connect
time and the **broker** publishes it when the gateway stops answering keepalives,
which is what clears `is_connected` without the middleware polling anything.

The gateway reads `mac` from the ESP-NOW frame header rather than the payload, so
a bracelet cannot report under another one's identity. MQTT credentials are broker
credentials, not Supabase keys; keep Supabase access credentials out of the
firmware.

```bash
cd esp32-gateway
idf.py set-target esp32
idf.py menuconfig   # Wi-Fi SSID/password, MQTT broker URI/credentials, gateway location
idf.py -p PORT flash monitor
```

On connect the firmware logs `Associated on channel N`. Setting the same channel
as `CONFIG_ESPNOW_CHANNEL` on the bracelets only saves them the first sweep;
they find the gateway either way.

### Local Python MQTT broker and middleware

The C6 bracelet does **not** connect to the PC hotspot directly: bracelet traffic
reaches the ESP32 gateway over ESP-NOW. The gateway connects to the PC hotspot
over Wi-Fi and sends MQTT to this Python broker.

On the PC, create a virtual environment and install the middleware dependencies:

```powershell
cd mqtt_middleware
py -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
```

Copy `.env.example` to `.env` and fill in your Supabase project URL and service-role
key. The middleware loads this file automatically; `.env` is Git-ignored:

```powershell
if (!(Test-Path .env)) { Copy-Item .env.example .env }
notepad .env
```

Replace `192.168.137.1` with the PC's hotspot IPv4 address if it differs
(check `ipconfig`). Binding to the hotspot interface limits the unauthenticated
MQTT listener to that interface; allow inbound TCP port `1883` through Windows
Firewall for the hotspot's private network profile if required. This broker is
intended for a trusted local network and does not enable TLS or authentication.

In `esp32-gateway`, set the Wi-Fi SSID/password to the PC hotspot and set
*Gateway Configuration → MQTT broker URI* in `idf.py menuconfig` to
`mqtt://192.168.137.1:1883` (use the same PC address). Leave the MQTT username
and password empty.

Find the URL and **service-role key** in the Supabase project's API settings; the
service-role key is secret and must never be committed or placed in the ESP
firmware. After filling in `.env`, start the middleware from the `mqtt_middleware`
directory:

```powershell
python server.py --bind 192.168.137.1
```

The middleware subscribes to five topics and does one thing with each message:

| Topic | What it does |
| --- | --- |
| `savi/id/request` | Looks up (or creates) the `braceletes` row for that MAC and publishes its `esp_id` |
| `savi/telemetry` | Upserts `braceletes` on `mac` (`ON CONFLICT (mac) DO UPDATE`) |
| `savi/alert` | Inserts into `alertas`, resolving `esp_id` from the MAC |
| `savi/gateway` | Upserts `gateways` on `mac` with the battery, bracelet count, and `is_connected = true` |
| `savi/gateway/offline` | Clears `is_connected` for that gateway |

PostgreSQL owns `esp_id`, so an ID request is a lookup, not an allocation: the
same MAC always gets the same ID back, which is what lets a bracelet roam between
gateways without changing rows. At startup the middleware clears `is_connected` on
every gateway, since anything still flagged connected is left over from the last
run. Run [`schema.sql`](./schema.sql) in the Supabase SQL Editor first. Supabase
errors are reported in the middleware console.

The default bind address is `0.0.0.0`; prefer specifying the hotspot interface
address when running on a PC with other network interfaces.

---

## ESP-NOW protocol

Packed little-endian structs shared by both firmwares in
[`savi_proto.h`](./components/savi_proto/include/savi_proto.h). The first two
bytes are `version` and `type`; both ends reject a mismatched version, so bump
`SAVI_PROTO_VERSION` (currently **7**) and reflash bracelet and gateway together
after any layout change.

| `type` | Name | Direction | Payload |
| --- | --- | --- | --- |
| 1 | `SAVI_MSG_SENSOR_DATA` | bracelet → gateway, unicast | `esp_id`, `temperature_c`, `hr_bpm`, `spo2_pct`, `battery_pct`, `ax ay az` (g), `gx gy gz` (dps) |
| 2 | `SAVI_MSG_SENSOR_EVENT` | bracelet → gateway, unicast | `esp_id`, `event_id`, `value` |
| 3 | `SAVI_MSG_HELLO` | bracelet → broadcast | header only |
| 4 | `SAVI_MSG_WELCOME` | gateway → broadcast | `channel`, `bracelet_mac` |
| 5 | `SAVI_MSG_NEED_ID` | bracelet → gateway, unicast | header only |
| 6 | `SAVI_MSG_ASSIGN_ID` | gateway → broadcast | `esp_id`, `bracelet_mac` |

Two separate conversations. `HELLO`/`WELCOME` is discovery: the gateway answers it
itself, in microseconds, so finding a gateway or replacing one that went out of
range never touches the middleware. `NEED_ID`/`ASSIGN_ID` is identity, sent only
while `esp_id` is 0, and the gateway has to ask the middleware over MQTT — which
is why it is kept off the failover path.

Gateway replies are broadcast and carry `bracelet_mac`. A bracelet learns a
gateway's MAC from the frame header, and ESP-NOW only lets you unicast to a peer
you already hold, so broadcasting the reply means the gateway needs no peer
bookkeeping at all: it keeps one peer, the broadcast address, however many
bracelets it serves. Every other bracelet ignores a reply addressed elsewhere.

Events: `SAVI_EVENT_TEMP_HIGH = 1` (`value` is °C), `SAVI_EVENT_MOTION = 2`
(`value` is \|a\| in g), `SAVI_EVENT_BATTERY_LOW = 3` (`value` is percent).
`battery_pct` is `SAVI_BATTERY_PCT_UNKNOWN` (255) when there is no gauge.

### No encryption on this hop

ESP-NOW traffic is plaintext, deliberately. ESP-NOW cannot encrypt broadcast at
all, and its unicast encryption needs a PMK/LMK pair shared by every device —
which stops being a secret the moment it ships in the firmware, where it also
gave no authentication and no per-device isolation. Rather than look protected
while the key sat in the repo, the keys are gone: anyone with a radio in range can
read vitals off the air, and that is written down here instead of implied.

TLS on the MQTT hop (`mqtts://`) is unaffected and still worth using — it protects
a different link, gateway to broker, with real certificates.

If this ever needs confidentiality on the radio, the honest version is per-device
keys provisioned into NVS plus flash encryption, not a shared constant in
`Kconfig.projbuild`.

Both devices must share the same 2.4 GHz channel; a bracelet that gets no answer
sweeps channels 1–13 until it does.

`components/savi_proto/proto_check.c` is a host program, not firmware. Compile and
run it to assert the struct sizes after editing the header:

```bash
gcc -I components/savi_proto/include -o proto_check components/savi_proto/proto_check.c && ./proto_check
# sensor_data=37 event=11 hello=2 welcome=9 need_id=2 assign_id=12
```

---

## Supabase schema (expected)

Create the PostgreSQL tables in the Supabase SQL Editor using [`schema.sql`](./schema.sql).
The Python middleware writes telemetry to `braceletes` and alert rows to
`alertas` using the Supabase service-role key. Keep this key on the PC running
the middleware; never put it in the gateway or bracelet firmware.

| Table | Contents |
| --- | --- |
| `braceletes` | One row per bracelet: `esp_id` (identity PK), `mac` (unique), `em_uso`, latest vitals, `bateria`, `atualizado_em` |
| `alertas` | Append-only alert log referencing `braceletes.esp_id` |
| `gateways` | One row per gateway: `gateway_id`, `mac`, `local`, `bateria`, `is_connected`, `braceletes` (bracelets streaming to it now), `atualizado_em` |

`esp_id` is the key to search and join on, and the one a bracelet asks for.
PostgreSQL generates it; `mac` is how the middleware finds the right row, which is
why it is `UNIQUE`. `em_uso` is yours to manage (is this bracelet on a patient?):
the middleware never writes it. Both `atualizado_em` columns default to `NOW()`,
so a row is still stamped when a gateway's clock has not synced yet.

`schema.sql` is idempotent and also migrates an older database: it adds `mac` and
`bateria`, converts `esp_id` to an identity column and pushes the sequence past
the highest existing ID. Rows left from the old ID pool have `mac IS NULL` and no
bracelet will ever match them; the file ends with the two statements that delete
them and make `mac` strict. Nothing writes to `gateways` yet; it is there for the
dashboard.

```sql
POST /rest/v1/braceletes?on_conflict=mac&select=esp_id
Prefer: return=representation,resolution=merge-duplicates
{"mac":"aa:bb:cc:dd:ee:ff","temperatura":36.5,"bpm":72,"spo2":98,"bateria":64,"acc_x":0.1,"acc_y":0.2,"acc_z":0.3,"gyro_x":1.0,"gyro_y":2.0,"gyro_z":3.0,"atualizado_em":"...-03:00"}

POST /rest/v1/alertas
{"esp_id":1,"tipo":"MOTION","valor":1.2}
```

The MQTT client and message formats are implemented in
[`mqtt_gateway.c`](./esp32-gateway/main/mqtt_gateway.c); the Python middleware
maps telemetry and alert payload fields into these existing Supabase tables.

---

## Power (bracelet, estimated)

Not measured. Duty cycle from firmware + typical datasheet currents:

| State | Rough average |
| --- | --- |
| Idle after ID, radio in min modem sleep | ~35–50 mA → **35–50 mAh per hour** |
| Motion (LED 50% + active buzzer) | add ~15–30 mA while the alert is on |

The radio dominates. MAX30102 LEDs stay pulsing for HR/SpO2. LED/buzzer milliamps depend on the hardware (series resistor / buzzer type).

---

## Bring-up checklist

1. Run `schema.sql` in Supabase; configure `SUPABASE_URL` and `SUPABASE_SERVICE_ROLE_KEY` in the middleware terminal, then start the middleware.
2. Set the gateway Wi-Fi to the PC hotspot and its MQTT URI to the PC hotspot IP; flash it and wait for `Got IP`, `Connected to MQTT broker`, and `Associated on channel N`.
3. Optionally set the C6 `CONFIG_ESPNOW_CHANNEL` to **N** to skip the first sweep, then flash the bracelet.
4. First boot: the bracelet logs `Using gateway ...` then `Assigned esp_id=N`, and the middleware logs `Gave esp_id=N to bracelet aa:bb:...` followed by telemetry.
5. Within a minute the gateway's own row appears in `gateways` with `is_connected = true` and its bracelet count.
6. More gateways: flash each with the same MQTT broker URI and its own *local*. Bracelets find them on their own; no bracelet reflash is needed.
