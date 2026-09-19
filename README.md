# ESP32 Shopify Order Notifier

A desk device that watches my Shopify store ([isotropicclothing.com](https://isotropicclothing.com))
and reacts when an order comes in: it chimes, lights up, and shows the day's
sales on an OLED. A button cycles through stats pages.

> **TODO: photo of the device running here** — put images in `docs/` and link them
> like `![The device running](docs/device.jpg)`.
> **TODO: short clip of a new order arriving (chime + green animation + NEW ORDER bar).**

## Why I built it

I'm an electrical engineering grad looking for firmware roles. My other projects
cover bare-metal peripherals and PCB design, but not connectivity or the
practices that separate a demo from something you'd ship. This one was chosen to
cover exactly that gap:

- TLS with real certificate verification (not disabled checks)
- Credentials provisioned into NVS instead of compiled into the binary
- OTA updates with automatic rollback, proven by shipping deliberately broken firmware
- Reconnect logic, exponential backoff, and graceful degradation when a peripheral fails

Every driver here (display, audio, LEDs, button) is written from scratch against
the datasheet or peripheral API rather than pulled from a library, so I can
explain each one line by line.

## What it does

| | |
|---|---|
| **Polls** | Shopify GraphQL Admin API every 60 s for today's orders |
| **On a new order** | two-note chime, green LED animation, `NEW ORDER #xxxx` bar on screen, button ring lights until acknowledged |
| **Screen pages** | today's overview, revenue (with average order value), units sold, shipped, device diagnostics |
| **Diagnostics page** | Wi-Fi RSSI, uptime, lowest-ever free heap, running firmware version |
| **Offline** | keeps showing the last known stats, marked `OFFLINE`, first LED dim red |
| **Long press** | checks for a firmware update and installs it |

"Today" means midnight in local time (Eastern, with daylight saving handled), not
UTC, which the Shopify API returns.

## Hardware

- ESP32-WROOM-32 devkit (4 MB flash, PCB trace antenna)
- 2.42" OLED, 128x64, SSD1309, SPI
- MAX98357A I2S amplifier + 40 mm 4 Ω 3 W speaker
- WS2812B strip, cut to 8 LEDs
- 12 mm momentary push button with LED ring
- 1N4148 diode, 1000 µF electrolytic cap
- 2 mm frosted acrylic as an LED diffuser

Assembled on perfboard, with the ESP32, OLED, and amplifier on female headers so
they stay removable. The speaker, LED strip, and button connect through headers
too, since they mount to the enclosure rather than the board.

### Wiring

| Peripheral | Signal | ESP32 pin |
|---|---|---|
| OLED | SCL (clock) | GPIO 18 |
| OLED | SDA (MOSI) | GPIO 23 |
| OLED | RES | GPIO 19 |
| OLED | DC | GPIO 16 |
| OLED | CS | GPIO 17 |
| OLED | VCC / GND | 3V3 / GND |
| Amplifier | BCLK | GPIO 26 |
| Amplifier | LRC | GPIO 25 |
| Amplifier | DIN | GPIO 22 |
| Amplifier | VIN / GND | VIN (5 V) / GND |
| LED strip | DIN | GPIO 27 |
| LED strip | 5 V | VIN through 1N4148 (≈4.3 V) |
| LED strip | GND | GND, on its own wire back to the ESP32 |
| Button | switch | GPIO 32 to GND (internal pull-up) |
| Button | LED ring | GPIO 33 direct (this ring has a built-in resistor) |

Plus a 1000 µF cap across the strip's 5 V and GND, mounted next to the strip's
connector.

Notes on the choices:

- **GPIO 6–11** are the SPI flash and unusable. **GPIO 34–39** are input-only with
  no pull-ups, so the button can't go there. **GPIO 0, 2, 12, 15** are strapping
  pins and are avoided for anything driven at boot.
- The WS2812B datasheet wants a logic high above ~3.5 V but the ESP32 outputs
  3.3 V. A 1N4148 in series with the strip's 5 V supply drops it to about 4.3 V,
  which brings the threshold under the ESP32's output. The firmware caps
  brightness so the strip never draws more than the diode can carry.
- The 1000 µF cap absorbs the current spike when LEDs switch on, which otherwise
  browns out the ESP32.
- The amplifier and LED strip each get their own ground wire back to the ESP32
  rather than sharing the middle of the ground bus, to keep their current pulses
  off the display's signal ground.

## Firmware architecture

ESP-IDF v5.5.5. One module per subsystem, in `firmware/main/`:

| File | Responsibility |
|---|---|
| `main.c` | polling loop, screen pages, button actions, OTA self-test |
| `creds.c` | reads credentials from NVS at boot |
| `wifi.c` | station mode, event-driven connect/reconnect |
| `time_sync.c` | SNTP, local timezone, start-of-local-day calculation |
| `shopify.c` | token exchange and the orders query over verified TLS |
| `oled.c` | SSD1309 driver: framebuffer, 5x7 font, page addressing |
| `audio.c` | I2S output, chime synthesised as sine waves with an envelope |
| `leds.c` | WS2812B driver over RMT, animations, brightness cap |
| `button.c` | sampling debounce, short and long press |
| `ota.c` | manifest check, download, rollback control |

Design points worth calling out:

- **Network work runs in its own task** with an 8 KB stack, because TLS needs far
  more stack than `app_main` gets by default.
- **Audio and LEDs each own a task** so a 0.7 s chime or a 2.6 s animation never
  blocks polling. Repeated triggers coalesce instead of queueing up.
- **The display is shared between two tasks** (polling and button), so all access
  goes through a mutex, and each poll fetches into a local struct that's swapped
  in atomically. Without that, the button task could redraw from half-updated data.
- **Every peripheral is optional.** If the OLED, amplifier, LEDs, or button fail to
  initialise, the device logs it and carries on with the rest.
- **Money is handled in integer cents**, never floating point.
- **Timeouts use `esp_timer`** (monotonic since boot) rather than the wall clock,
  which jumps when SNTP first syncs.

## Security design

- **Minimum scope.** The Shopify app requests `read_orders` only — not
  `read_all_orders`, not any write scope. If the credentials leak, an attacker can
  read roughly 60 days of orders and nothing else.
- **Real certificate verification.** Every request uses `esp_crt_bundle`, so the
  server's certificate is checked against trusted authorities before any secret is
  sent. Verification is never disabled.
- **No secrets in the firmware or the repo.** Credentials are generated into an NVS
  image on the laptop and flashed once to `0x9000`. The firmware reads them at boot.
  Normal firmware flashing never touches that partition, so they survive updates.
  This matters because OTA means the binary is published publicly.
- **Verified, not assumed.** Every release is checked with
  `Select-String -Path firmware\build\firmware.bin -Pattern "<client id>" -SimpleMatch`,
  which must return nothing.
- **Nothing sensitive is logged.** No passwords, tokens, or secrets reach the serial
  output. The client secret is wiped from its stack buffer right after the request.

### Known limitation: flash encryption is not enabled

Credentials are stored unencrypted in flash, so someone with physical access to the
device and `esptool` could read them. Fixing that requires flash encryption, which
permanently burns eFuses on the ESP32. This build runs on my only board with a
usable antenna, so I chose not to risk it rather than ship an untested claim.
Enabling flash encryption plus NVS encryption is the natural next step on dedicated
hardware, and the partition table already reserves an `nvs_keys` partition for it.

## OTA updates and rollback

Partition table: no factory partition, two 1.8 MB OTA slots, `otadata`, `nvs`,
`phy_init`, and `nvs_keys`. Planned this way from the first commit, because
retrofitting OTA onto a 4 MB chip later means repartitioning a working device.

The update flow:

1. A long press fetches `manifest.json`, which names the newest version and the
   binary's URL.
2. If that version differs from the running one, the binary downloads into the
   unused slot, with progress on the OLED.
3. `esp_https_ota_finish` validates the image and marks the new slot pending verify.
4. The device reboots into it. The new firmware must complete one successful poll —
   proving Wi-Fi, TLS, and the Shopify API all work — before calling
   `esp_ota_mark_app_valid_cancel_rollback`.
5. If it crashes, or three polls fail, it calls
   `esp_ota_mark_app_invalid_rollback_and_reboot` and the bootloader returns to the
   previous firmware.

**Tested by deliberately breaking it.** Release 0.3.0 was published with a wrong
Wi-Fi password on purpose. The device installed it, failed its self-test three
times, and rolled back to 0.2.0 on its own with no USB cable involved. That commit
is intentionally still in the history.

## Debugging notes

Problems worth recording, since the fixes weren't obvious:

- **GitHub release-asset URLs don't work for OTA here.** `releases/latest/download/...`
  responds with a redirect, and `esp_http_client` failed with `Out of buffer` even
  after raising the receive buffer to 8 KB. Serving the manifest and binary directly
  from `raw.githubusercontent.com` works. The underlying cause is still open (below).
- **`rmt_tx_wait_all_done` times out** on every LED frame, even though the LEDs
  display correctly. The wait was removed: one frame takes ~0.2 ms and frames are
  20 ms apart, so there's nothing to wait for. Also open.
- **`-Wformat-truncation` is an error in ESP-IDF.** A `char buf[8]` holding `"%d%%"`
  fails to compile, because the compiler assumes any `int`, not 0–100. Useful, since
  a small buffer would silently truncate instead of crashing.
- **Serial port conflicts.** `Access is denied` on the COM port means a serial monitor
  is still holding it; `doesn't exist` means the board is unplugged or was
  re-enumerated somewhere else.
- **Default flash size is 2 MB.** The boot log warned that the detected flash
  (4096k) was larger than the image header claimed (2048k) until the project was
  configured for 4 MB, which OTA needs.

## Open questions

- Why does `rmt_tx_wait_all_done` never report completion? Next step is capturing
  the data line with a logic analyser and checking whether the transmission genuinely
  completes.
- Why did the GitHub redirect exhaust the HTTP client's buffer at 8 KB? The signed
  redirect URL should be around 1 KB.
- Does `nvs_flash_init` pick up the `nvs_keys` partition automatically once NVS
  encryption is enabled, or is an explicit secure-init call needed?

## Repository layout

```
firmware/          ESP-IDF project (partitions.csv, version.txt, main/)
ota/firmware.bin   the binary the device downloads
manifest.json      version + URL the device checks
tools/probe.py     laptop script that tests the Shopify API independently
tools/creds.example.csv   credential template (placeholders only)
```

## Building and provisioning

```bash
# 1. Build (ESP-IDF v5.5.x)
idf.py set-target esp32
idf.py build

# 2. Create the credential image from a CSV kept outside the repo
python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
    generate creds.csv nvs.bin 0x6000

# 3. Flash the credentials once, then the firmware
esptool.py -p PORT write_flash 0x9000 nvs.bin
idf.py -p PORT flash monitor
```

`tools/creds.example.csv` shows the expected keys. `idf.py erase-flash` wipes the
credentials, so step 3's first command has to be repeated after one.

### Cutting a release

1. Bump `firmware/version.txt`
2. `idf.py build`
3. Check the binary contains no secrets (`Select-String`, above)
4. `copy firmware\build\firmware.bin ota\firmware.bin`
5. Set the same version in `manifest.json`, commit, push
6. Long press the button on the device

The version string in `manifest.json` must match `version.txt` exactly; the
comparison is a plain string match, so `0.4.0` and `v0.4.0` would loop forever.

## Reliability

> **TODO: fill in after a multi-day soak test on the perfboard build — uptime,
> whether lowest-ever heap stayed flat, that the daily stats reset at local
> midnight, and that the ~23 h access token refresh runs cleanly. The token
> refresh path has not yet executed on hardware.**

Free heap sits around 180 KB with Wi-Fi, TLS, and all peripherals running. The
lowest-ever figure (roughly 125–130 KB, reached during a TLS handshake) is shown on
the device page, so a slow leak would be visible without a serial cable.

## Status and next steps

Working: everything described above, assembled on perfboard with the modules on
removable headers.

Still to do:
- Multi-day soak test on the assembled build
- Cut the acrylic diffuser and build an enclosure
- Flash and NVS encryption, on dedicated hardware
