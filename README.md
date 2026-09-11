# SAVI

Bracelet nodes and a Wi-Fi gateway that collect vitals over **ESP-NOW** and write them to **MySQL**.

```
MAX30102 / MAX30205 / BMI270
        │ I2C
   ESP32-C6 bracelet  ──ESP-NOW──►  ESP32 gateway  ──TCP──►  MySQL
   LED + buzzer                     (STA on AP)
```

| Folder | Role | Intended chip |
| --- | --- | --- |
| `esp32-c6/` | Wearable node: sensors, motion alert, ESP-NOW TX | ESP32-C6 |
| `esp32-gateway/` | Receives ESP-NOW, assigns IDs, updates the database | ESP32 (or other STA-capable ESP) |

Requires [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c6/get-started/index.html). GPIO and I2C use `esp_driver_gpio` and `esp_driver_i2c` (not the old `driver` component name).

---

## Bracelet (`esp32-c6`)

Samples three I2C sensors, drives a motion LED/buzzer, and talks to the gateway.

| Function | Part | Notes |
| --- | --- | --- |
| Heart rate / SpO2 | MAX30102 (`0x57`) | SpO2 mode, 50 Hz, red + IR |
| Skin temperature | MAX30205 (`0x48`) | One-shot about every 250 ms |
| Accel / gyro | BMI270 (`0x68`) | 50 Hz, advanced power save after config |
| Motion LED | GPIO 4 | 200 ms blink while motion is latched |
| Buzzer | GPIO 5 | Held high for the whole motion interval |

Default I2C: **SDA GPIO 6**, **SCL GPIO 7**, 400 kHz. Change pins in `esp32-c6/main/sensors.h`.

### Runtime

1. Load `esp_id` from NVS (`savi` / `esp_id`).
2. If it is `0`, broadcast `MSG_NEED_ID` every 2 s until `MSG_ASSIGN_ID` arrives, then store the id.
3. Every **50 ms**: read IMU (and HR FIFO), detect motion (`|a|` more than **0.4 g** from 1 g), blink/buzz, fire `EVENT_MOTION_DETECTED` on the rising edge.
4. Temperature ≥ **38 °C** fires `EVENT_TEMP_HIGH` on the rising edge (temp is refreshed ~250 ms).
5. Every **1 s** (once an id exists): send a packed `MSG_SENSOR_DATA` frame.

Radio: Wi-Fi **PS none** until an id is assigned so the assign packet is not missed; then **min modem sleep**. Sensor reports go to broadcast `FF:FF:FF:FF:FF:FF`.

Set **ESP-NOW channel** in `idf.py menuconfig` → *Example Configuration* so it matches the **access-point channel the gateway joined** (the gateway logs that channel at boot).

```bash
cd esp32-c6
idf.py set-target esp32c6
idf.py menuconfig   # Example Configuration → ESP-NOW channel = gateway AP channel
idf.py -p PORT flash monitor
```

---

## Gateway (`esp32-gateway`)

Connects as a Wi-Fi station (modem sleep off so ESP-NOW RX stays up), then:

| Incoming | Action |
| --- | --- |
| `MSG_NEED_ID` | Allocate an unused `esp_id` from MySQL (`em_uso=0`), mark it used, send `MSG_ASSIGN_ID` unicast |
| `MSG_SENSOR_DATA` | `UPDATE braceletes SET temperatura, bpm, spo2, acc_*, gyro_* WHERE esp_id=…` |
| `MSG_SENSOR_EVENT` `MOTION` | `INSERT INTO alertas (esp_id, tipo, valor)` |

Unassigned packets (`esp_id == 0`) trigger the same ID assignment path.

Configure SSID, password, and MySQL host/user/database in `idf.py menuconfig` → *Example Configuration*. Auth uses `mysql_native_password`.

```bash
cd esp32-gateway
idf.py set-target esp32
idf.py menuconfig   # Wi-Fi SSID/password, MySQL host/user/db
idf.py -p PORT flash monitor
```

On connect the firmware logs `STA channel N`. Flash every C6 with that same ESP-NOW channel.

---

## ESP-NOW protocol

Packed little-endian structs. First byte is `type`. Keep both copies of `espnow_example.h` in sync.

| `type` | Name | Payload |
| --- | --- | --- |
| 1 | `MSG_SENSOR_DATA` | `esp_id`, `temperature_c`, `hr_bpm`, `spo2_pct`, `ax ay az` (g), `gx gy gz` (dps) |
| 2 | `MSG_SENSOR_EVENT` | `esp_id`, `event_id`, `value` |
| 3 | `MSG_NEED_ID` | single byte |
| 4 | `MSG_ASSIGN_ID` | `esp_id` |

Events: `EVENT_TEMP_HIGH = 1` (`value` is °C), `EVENT_MOTION_DETECTED = 2` (`value` is \|a\| in g).

ESP-NOW is unencrypted. Both devices must share the same 2.4 GHz channel.

---

## MySQL schema (expected)

```sql
-- braceletes: one row per wearable; em_uso=0 means the id can be handed out
UPDATE braceletes SET
  temperatura=…, bpm=…, spo2=…,
  acc_x=…, acc_y=…, acc_z=…, gyro_x=…, gyro_y=…, gyro_z=…
WHERE esp_id=…;

INSERT INTO alertas (esp_id, tipo, valor) VALUES (…, 'MOTION', …);

SELECT esp_id FROM braceletes WHERE em_uso=0 ORDER BY esp_id ASC LIMIT 1;
UPDATE braceletes SET em_uso=1 WHERE esp_id=… AND em_uso=0;
```

SQL strings live in `esp32-gateway/main/mysql_db.c`.

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

1. MySQL reachable from the gateway AP; `mysql_native_password` user; `braceletes` / `alertas` columns as above; spare rows with `em_uso=0`.
2. Flash gateway, wait for `Got IP` and `STA channel N`.
3. Set the C6 `CONFIG_ESPNOW_CHANNEL` to **N**, then flash the bracelet.
4. First boot: bracelet logs `requesting from master`; gateway logs `Assigned esp_id`; then 1 Hz `RX sensor` lines and MySQL updates.
