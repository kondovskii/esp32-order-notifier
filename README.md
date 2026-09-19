# Soundpod — A Handmade Portable MP3 Player

A battery-powered, pocket-sized MP3 player built from individual components on an Arduino Pro Mini — no pre-made audio shield, no kit. Every circuit was designed, wired, and soldered by hand, with a custom SPI OLED interface and a fully original dancing-cat boot animation.

![Soundpod running](./photos/main-screen.jpg)

## Features

- 🎵 MP3 playback from a microSD card via DFPlayer Mini
- 🖥️ 128x64 SPI OLED display showing track number, play/pause status, and volume
- 🐱 Custom animated boot sequence — a dancing cat in headphones, followed by a personalized welcome message
- 🎶 Auto-advances to the next track when a song finishes
- 🔋 Rechargeable via USB-C through a TP4056 LiPo charging circuit, with full overcharge/over-discharge/short-circuit protection
- 🎚️ 5-button control: play/pause, next, previous, volume up, volume down

## Hardware

| Component | Part |
|---|---|
| Microcontroller | Arduino Pro Mini (5V / 16MHz, ATmega328) |
| Audio decoder | DFPlayer Mini MP3 module |
| Storage | MicroSD card (FAT32, SanDisk recommended) |
| Display | Adafruit 0.96" SPI OLED (SSD1306, 128x64) |
| Battery | EEMB 3.7V 1100mAh LiPo |
| Charging | TP4056 (HW-373, with DW01A + FS8205A protection) |
| Audio out | Kycon 3.5mm right-angle stereo jack (STX-3000) |
| Controls | 5x tactile pushbuttons |
| Programming | SparkFun Serial Basic Breakout (CH340) |

## Wiring

### Programming interface (Serial Basic ↔ Pro Mini)

| Serial Basic | Pro Mini |
|---|---|
| DTR | GRN |
| TXO | RXI |
| RXI | TXO |
| VCC | VCC *(only needed when not running on battery)* |
| GND | GND |

### OLED display (SPI)

| OLED pin | Pro Mini pin |
|---|---|
| GND | GND |
| VIN | VCC |
| CLK | D13 |
| DATA | D11 |
| DC | D9 |
| RST | D8 |
| CS | D10 |
| 3Vo | *not connected (regulated 3.3V output, unused)* |

> **Note:** This board's SPI/I2C bus mode is set by two jumpers (J1/J2) on the back. It ships defaulted to I2C — both jumpers must be cut to enable SPI mode.

### DFPlayer Mini

| DFPlayer pin | Pro Mini pin |
|---|---|
| VCC | VCC (5V) |
| GND | GND |
| RX | D3 |
| TX | D2 |
| DAC_L | Headphone jack Tip |
| DAC_R | Headphone jack Ring |
| GND | Headphone jack Sleeve |

Uses `SoftwareSerial` on D2/D3 so the DFPlayer never conflicts with the Pro Mini's single hardware serial port, which stays reserved for programming.

### Buttons

| Button | Pro Mini pin |
|---|---|
| Play / Pause | D4 |
| Next track | D5 |
| Previous track | D6 |
| Volume up | D7 |
| Volume down | D12 |

Each button's other leg ties to a shared GND rail. Wired with `INPUT_PULLUP`, so no external resistors are needed.

### Power (TP4056 → Pro Mini)

| TP4056 | Pro Mini |
|---|---|
| OUT+ | VCC |
| OUT− | GND |

The Serial Basic Breakout's own VCC line is left disconnected during normal battery-powered use — only GND/DTR/RXI/TXO stay connected, so USB can still be plugged in for reprogramming without double-powering the board.

## Code

The full sketch lives in [`soundpod.ino`](./soundpod.ino). Libraries required:

- `Adafruit_GFX`
- `Adafruit_SSD1306`
- `SoftwareSerial` (built-in)
- `DFRobotDFPlayerMini`

Before uploading, update `totalTracks` in the sketch to match the number of MP3 files on your SD card, and set `OWNER_NAME` to personalize the welcome message.

MP3 files must be named `0001.mp3`, `0002.mp3`, etc., placed in the root of a FAT32-formatted microSD card.

## Build challenges

A few of the harder problems solved along the way, for anyone hitting the same walls:

- **OLED showed nothing despite "successful" code execution.** SPI has no acknowledgment signal, so `display.begin()` reporting success doesn't confirm the display is actually receiving data — it just means the library didn't detect an error. Root cause ended up being twofold: the Serial Basic Breakout was configured for 3.3V output instead of 5V (a solder-jumper setting, not a switch), which meant every downstream pin was undervolted; and separately, the OLED module's SPI/I2C bus-select jumpers were left in their factory default (I2C) while the code was written for SPI.
- **DFPlayer wouldn't auto-advance tracks.** Fixed by listening for the module's own `DFPlayerPlayFinished` serial event, rather than relying on manual button presses between songs.
- **Tactile buttons intermittently failed to register.** 4-legged tactile switches have two internally pre-bridged pin pairs; a wire pair chosen from the same side reads as permanently "pressed" or never registers a press at all. Confirmed correct pairing with a multimeter continuity check before wiring into the final circuit.

## Photos

*(Add build photos here — breadboard prototype, soldering process, final assembly)*

## License

Personal project, built for fun and for learning. Feel free to reference the wiring or code for your own build.
