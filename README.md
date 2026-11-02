# CoCIS Smart Control & Monitoring System

ESP32 + ESP-IDF (PlatformIO) project, simulated in Wokwi. Access control, occupancy monitoring, automatic lighting, SD logging, configurable access schedule and a serial admin CLI for the CoCIS pilot (2 lecture rooms + main entrance).

## 1. Architecture (what changed from the original proposal)

| Original proposal | Now (Wokwi free tier) |
|---|---|
| 3 ESP32 terminals (Room 1, Room 2, Entrance) talking over a network | **1 ESP32**, every terminal is a FreeRTOS task |
| Inter-board messaging (ESP-NOW/MQTT/UART) | In-RAM **queues, mutexes, event group** (`system_sync.*`) |
| `is_access_permitted()` called from Member 1 only | Also called from the system monitor (intrusion detection) |
| Each member tests alone with a throwaway `main.c` | **Shared `main.c`** + `system_sync_init()` start everything |

The ESP32 is dual-core and tasks are **not pinned** (`TASK_CORE = tskNO_AFFINITY` in `system_sync.h`), so tasks really can run in parallel: every shared resource must be protected.

## 2. Build & run

```
pio run                 # build
pio run -t clean        # REQUIRED after adding/removing files in src/ (CMake globs src/*.*)
```
Then start the Wokwi simulator from VS Code (`Wokwi: Start Simulator`). Firmware path is in `wokwi.toml`.

## 3. Boot order (`src/main.c`)

1. `system_sync_init()`: mutexes, queues, event group, evacuation button, default clock
2. `access_control_init()`: creates the SPI3 bus (the SD card reuses it)
3. `lighting_control_init()`
4. `data_storage_init()`: **must be after step 2**
5. `schedule_init()`, `serial_cli_init()`
6. `system_sync_start_tasks()`: only now do tasks start

## 4. Tasks

| Task | Prio | Stack | Period | Owner |
|---|---|---|---|---|
| `vTaskAccessControl` | 5 | 4096 | 100 ms poll | Member 1 |
| `vTaskSystemMonitor` | 4 | 3072 | 50 ms | Member 3 |
| `vTaskLightingControl` | 3 | 3072 | 100 ms | Member 2 |
| `vTaskStorage` | 2 | 8192 | blocks on queues | Member 4 |
| `vTaskCLI` | 2 | 6144 | blocks on UART | Member 5 |

Higher number = higher priority. Created only in `system_sync.c`; do not call `xTaskCreate` elsewhere.

## 5. Synchronization rules (read before touching shared data)

**Lock order: `g_config_mutex` -> `g_sd_mutex` -> `g_spi_mutex`.** Never acquire in reverse. Never block (queue send with timeout, `vTaskDelay`) while holding a mutex. Never call another module while holding your own mutex.

| Object | Purpose |
|---|---|
| `g_spi_mutex` | Shared SPI3 bus (RFID readers + SD card) |
| `g_sd_mutex` | FAT filesystem / log file |
| `g_config_mutex` | Schedule settings, admin credentials |
| `g_access_queue` (`access_event_t`, len 16) | access_control -> storage |
| `g_syslog_queue` (`system_log_event_t`, len 16) | anyone (via `system_log_post()`) -> storage |
| `g_system_events` | Bits: `SYS_EVT_EVACUATION`, `SYS_EVT_INTRUSION`, `SYS_EVT_SD_READY` |

`vTaskStorage` is the **only** consumer of both queues = single writer to the SD card.
Producers send with timeout 0: a full queue drops the event with a warning instead of stalling a door.

## 6. Hardware / pin map

| Function | GPIO |
|---|---|
| SPI3 bus: SCK / MISO / MOSI | 18 / 19 / 23 |
| RFID reset (shared) | 15 |
| RFID ENTRY CS / EXIT CS | 5 / 4 |
| Door servos ENTRY / EXIT | 27 / 33 |
| PIR Room 1 / Room 2 | 13 / 14 |
| LED Room 1 / Room 2 | 25 / 26 |
| Evacuation pushbutton (to GND, internal pull-up) | 32 |
| SD card CS (planned, Member 4) | 2 |
| I2C LCD SDA / SCL (optional, Member 5) | 21 / 22 |
| Potentiometer (wired, currently unused) | 34 |

**LEDC allocation:** timer 0 + channels 0-1 = servos (50 Hz). Timer 1 + channels 2-3 = room LEDs (5 kHz). Do not reuse.

## 7. System behaviour

**Access decision (per scan, `access_control.c`):** unknown tag -> denied; outside open hours -> denied (**entry and exit**); entry while already inside -> denied (anti-passback); exit while not inside -> denied; otherwise granted: flag + headcount updated, that gate's servo opens 3 s. Same tag on the same reader within 2 s is ignored. Card colours in Wokwi: Blue/Green/Yellow/Key fob are whitelisted, **Red is deliberately unknown**, the grey NFC tag (7-byte UID) is unsupported.

**Lighting (per room):** motion -> 100%; motion stops -> 30% standby; 30 s without motion -> off; motion in standby -> 100%. Independent of RFID and headcount.

**Evacuation:** button (GPIO 32) or `system_evacuation_set(true, "cli")` sets `SYS_EVT_EVACUATION`. The monitor task then holds both doors open and forces both rooms to 100%. Clearing restores normal behaviour.

**Intrusion:** monitor task: building closed (`!is_access_permitted()`) + PIR motion -> `SYSLOG_INTRUSION` (rate-limited 30 s per room) + `SYS_EVT_INTRUSION` bit.

**Clock:** system clock (`time(NULL)`) is the only time source, TZ = EAT (UTC+3), use `localtime_r()`. Defaults to 2026-11-02 09:00 at boot; the CLI changes it with `settimeofday()`.

## 8. Module status

| Module | Files | Status |
|---|---|---|
| Access control (M1) | `access_control.c/.h` | Done (anti-passback, evacuation latch). Role-based privileges: TODO |
| Lighting (M2) | `lighting_control.c/.h` | Done (2-stage dimming, getters, evacuation) |
| Sync core (M3) | `system_sync.c/.h`, `main.c` | Done |
| Storage + schedule (M4) | `data_storage.h`, `schedule.h` | **Stub** (`stub_data_storage.c`, `stub_schedule.c`) |
| Serial CLI (M5) | `serial_cli.h` | **Stub** (`stub_serial_cli.c`) |

## 9. Guidelines for Members 4 and 5

1. Read your header (`include/data_storage.h`, `schedule.h`, `serial_cli.h`). It lists exactly what you must implement and the locking rules.
2. Create your real `src/<module>.c`, **delete your `src/stub_<module>.c`**, then run `pio run -t clean && pio run`. Keeping both causes duplicate-symbol errors.
3. Keep the declared signatures. If you need a change, tell Member 3 first; other modules call them.
4. Log events with `system_log_post(type, "fmt", ...)`. Do not write to the SD card from anywhere except `vTaskStorage` / `data_storage_*`.
5. SD card: mount on the **existing** `SPI3_HOST` bus (do not call `spi_bus_initialize` again; tolerate `ESP_ERR_INVALID_STATE`), CS = GPIO 2, then set `SYS_EVT_SD_READY`. Take `g_sd_mutex` then `g_spi_mutex` around file access.
6. `is_access_permitted()` is called from two tasks: take `g_config_mutex` inside, keep it short, handle windows crossing midnight, and fail open if the clock was never set. Your strong definition replaces the weak default in `access_control.c` automatically.
7. CLI: you own admin authentication. `schedule_set()` and `set-time` must be refused unless the admin password was entered. Read state through the public getters (see `access_control.h`, `lighting_control.h`, `schedule.h`), never through module internals.
8. Only edit your own files. For `diagram.json` changes (new parts/wires), keep them small and announce them, since the file is shared.

## 10. Testing checklist (Wokwi)

1. Blue at ENTRY -> granted, headcount 1, entry door opens 3 s.
2. Blue at ENTRY again -> denied (already inside).
3. Red at ENTRY -> denied (unknown tag).
4. Blue at EXIT -> granted, headcount 0. Blue at EXIT again -> denied (not inside).
5. Click PIR 1 -> LED full; stop -> 30%; 30 s later -> off.
6. Evacuation button -> doors held open + both LEDs full; press again -> normal.
7. (After Member 4) set time outside hours -> scans denied both ways; motion -> `INTRUSION_ALERT`.

## 11. Known limitations / TODO

- Only 4-byte UIDs are supported; whitelist is hard-coded in `access_control.c`.
- Role-based privileges (lecturer/admin bypass schedule) not implemented.
- Potentiometer is wired but unused.
- Evacuation button must exist in `diagram.json` (GPIO 32 to GND); the CLI can also trigger it.