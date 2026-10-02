# Samsung USB-C FM Radio Adapter

An open-source RP2040 + SI4703 adapter designed to replace the FM functionality of Samsung's original FM-capable USB-C earphones while retaining compatibility with Samsung's built-in FM Radio app.

## Current Status

**Functional prototype / hardware-verified prototype.** The owner physically verified commit `a15fce261708a2150e12d6dc011ce3794606c2b7` on an RP2040-Zero, SI4703 and Samsung phone. Normal earphone mode, FM playback and tuning worked, without the previously observed severe tuning delay or continuous loud beep. This firmware was promoted to main in merge commit `c94294bc829e44bb5e68e271b6d399e4fa02be47` without changing its bytes.

These results apply to the tested setup. The exact phone model, Android version and FM app version remain undocumented; compatibility across Samsung devices has not been established.

## Overview / Why This Project Exists

The key feature is the connection to Samsung's original FM software: the built-in FM Radio app/service acts as the host and controller for an external SI4703 tuner through an RP2040 protocol bridge. The adapter provides the FM-radio role normally associated with compatible Samsung USB-C earphones.

No replacement Android FM Radio app is required. The project implements the necessary parts of the Samsung-compatible USB control protocol; it does not reproduce all USB Audio or HID functionality of the original earphones.

## How It Works

```text
Samsung built-in FM Radio app/service
                  |
          USB-C control / status
                  |
               RP2040
     (Samsung protocol bridge)
                  |
                 I2C
                  |
               SI4703
                  |
           analog FM audio
                  |
       3.5 mm earphones / antenna
```

RP2040 receives Samsung USB control requests, translates them into tuner operations, and returns frequency/RSSI status and notifications. Audio comes from the SI4703 analog output to the connected earphones. FM PCM audio is not sent through RP2040 to the phone over USB. The earphones also serve as the tuner antenna.

## Features

- Samsung built-in FM Radio app/service integration, with normal earphone mode verified on the owner's setup.
- FM frequency tuning through `safeTune()`.
- Volume and mute control.
- SEEK command support and RSSI reporting; reliable station detection remains a known issue.
- SI4703 analog FM audio output.
- BesCmd SET=161, GET=162 and QUERY=163 handling.
- SEEK (CMD7) and direct tune (CMD9) scheduled in the control callback and processed in the main loop.
- Two-stage notification through Interrupt IN Endpoint `0x85`.
- Existing RAM-only CMD9 DebugSnapshot instrumentation retained in the verified firmware.

## Hardware

- Waveshare RP2040-Zero.
- SI4703 FM tuner module.
- 3.5 mm earphones for analog audio and antenna use.
- USB-C connection to a Samsung Android device with the built-in FM Radio app.

The module's exact model, supply wiring and complete circuit schematic remain to be documented. Check the module's electrical requirements before wiring it.

## Pinout

| SI4703 connection | RP2040 GPIO |
| --- | --- |
| RESET | GPIO2 |
| SDIO / SDA | GPIO4 |
| SCLK / SCL | GPIO5 |

I2C runs at 100 kHz. A separate diagnostic sketch repeatedly found address `0x10` after moving the physical bus from GPIO8/9 to GPIO4/5; the full adapter was then verified with this mapping. This observation does not establish why the earlier wiring failed.

Initialization is deliberately ordered as follows:

```cpp
Wire.setSDA(SDA_PIN);
Wire.setSCL(SCL_PIN);
Wire.setClock(100000);
rx.setup(RESET_PIN, SDA_PIN);
```

In PU2CLR SI470X 1.0.5, the second `setup()` argument is the actual SDA pin. The library drives SDA LOW during tuner reset, then calls `Wire.begin()` to enable I2C. Do not add a preceding `Wire.begin()`: Arduino-Pico 6.1.1 returns early if Wire is already running, preventing this call from restoring SDA to the I2C function. The retained `DUMMY_INT` definition is unused.

## Build Environment

The established build environment for the verified baseline is:

| Component | Version / selection |
| --- | --- |
| Arduino core | Earle Philhower Arduino-Pico 6.1.1 |
| Board target | Waveshare RP2040 Zero |
| USB stack | Adafruit TinyUSB (`usbstack=tinyusb`) |
| Adafruit TinyUSB Library | 3.7.7, bundled with the core |
| Tuner library | PU2CLR SI470X 1.0.5 (`SI470X.h`) |
| I2C library | Wire, bundled with the core |
| Board menus | 200 MHz, 2MB/no FS, Small (`-Os`), no RTOS; remaining menus at defaults |

Open [firmware/fm_adapter/fm_adapter.ino](firmware/fm_adapter/fm_adapter.ino) in Arduino IDE and select the board and USB stack above. Install the external dependencies separately; their source is not vendored here.

With Arduino CLI and that core installed:

```sh
arduino-cli compile --fqbn rp2040:rp2040:waveshare_rp2040_zero:usbstack=tinyusb firmware/fm_adapter
```

The GPIO4/5 firmware compiled with 85,788 bytes of flash and 17,836 bytes of global RAM. These are recorded build results, not guarantees for other toolchain versions. Arduino-Pico 6.2.0 was also found on the development PC; it is not the documented reference environment.

## Usage

1. Wire the module using the pinout above and verify its power connections.
2. Compile the firmware using the reference environment and upload it using the RP2040 board's supported upload procedure.
3. Connect earphones to the SI4703 analog output/antenna connection and connect the adapter to the Samsung phone over USB-C.
4. Open Samsung's built-in FM Radio app and use its tuning, volume and mute controls. Normal earphone mode is verified; station search has the limitations below.

**During initialization and connection experiments, keep the earphones out of your ears.** Loud analog audio transients have occurred in abnormal states. Start listening at low volume after stable operation is established.

## Protocol and Firmware Notes

The firmware sets VID `0x04E8`, PID `0xA05B`, manufacturer string `Samsung` and product string `Samsung USB C Earphone`. These are interoperability settings, not evidence of official approval or complete descriptor reproduction.

| Request | Command | Current behavior |
| --- | --- | --- |
| SET=161 (`0xA1`) | 0 / 4 / 5 | Radio mute-based on/off / mute / volume |
| SET=161 (`0xA1`) | 7 / 9 | Schedule SEEK / direct tune |
| GET=162 (`0xA2`) | 8 / 13 / 17 | Stored volume / stored frequency / zero |
| QUERY=163 (`0xA3`) | Current handler | Stored success flag, fixed byte 1, frequency and RSSI |

The request command is in `wValue`, with its argument in `wIndex`. An ACK means the control request was acknowledged, not that tuning has completed. Unhandled SET commands can still reach the ACK path; other GET commands currently return the default value 1.

SEEK/direct tune use `pending_cmd` and `target_val`; GET/QUERY return stored state. Notify packets are `01 00 08 00 00` and `01 01 <frequency low> <frequency high> <RSSI>`. SEEK status processing uses a greater-than-60-ms condition after `rx.seek()` returns. The library's tuning/seek polling can block; the firmware is not fully non-blocking.

Frequency values normally use `10770 = 107.70 MHz`. The preserved `safeTune()` calculations are `(freq_val - 8750) / 20` for inputs above 5000 and `(freq_val - 875) / 2` otherwise. SEEK converts `rx.getFrequency()` using `channel = (fake_f - 8750) / 10`, then `real_f = 8750 + channel * 20`. These are the verified legacy implementation's existing conversions, not a claim about every SI470X release. `setBand(0)` and `setSpace(0)` select the 87.5–108 MHz band and 200 kHz spacing in the installed library.

CMD9 snapshots use format v1, 88 bytes, vendor IN `0xC0/0xD9`, `wValue=0x464D`, `wIndex=1`. The Android diagnostic project retained on main comes from the later v4 experiment and is not a matching reader for this firmware. It is a development tool, not a replacement FM app. A v1 reader remains on `historical/working-snapshot-v1`. No diagnostic code was removed for this documentation update.

## Project Structure

```text
firmware/fm_adapter/fm_adapter.ino  Verified legacy firmware
diagnostics/android-fm-debug/    Retained experimental Android diagnostic tool
docs/hardware/                  Hardware and wiring notes
docs/protocol/                  Protocol notes and historical recovery evidence
docs/images/                    Reviewed photos/screenshots when available
README.md                       Public project overview
PROJECT_STATE.md                Development status and evidence handover
AGENTS.md                       Repository working rules
LICENSE                         MIT License for this project's own code
.gitignore                      Local files and build-output exclusions
```

Original project materials remain in a separate archive. Raw logs, credentials and unreviewed images should not be committed; review identifying information before publishing excerpts.

## Known Issues

1. **Automatic seek / station detection.** Station validation uses a simple `last_rssi > 15` threshold and is not reliable. Incorrect station detection can leave subsequent searching unable to continue normally. Direct tune's success flag is set unconditionally and is not a reception-quality test.
2. **Samsung playback-mode handling.** The earphone/speaker selection UI does not always appear at the desired time when connecting the adapter. Normal earphone mode itself has been physically verified.
3. **Standalone operation.** The Samsung phone/app is currently the primary controller. There is no phone-free standalone radio UI.
4. **Audio transients.** Loud beep/pop/transient events occurred in abnormal initialization or connection states. The verified GPIO4/5 setup operated normally, but all power and error states have not been tested.
5. **Error handling and concurrency.** Library polling can block, the pending command storage is a single slot rather than a queue, and input/transfer validation is limited. Notify transfer return values are recorded for CMD9 diagnostics; state advances without a dedicated failure retry. Further review is needed before stronger robustness claims.

USB PCM audio, phone speaker/Bluetooth routing, full USB Audio functionality and OLED/standalone controls are not implemented features. Compatibility is limited to evidence from the tested setup.

## Roadmap

Future work; none of these items is claimed complete:

- Improve SEEK / RSSI station validation.
- Improve Samsung playback-mode handling.
- Remove obsolete diagnostic instrumentation after validation.
- Improve error handling / I2C recovery.
- Reduce audio transients during abnormal initialization.
- Optional standalone control mode.
- Optional SSD1306 OLED display on the I2C bus.
- Optional physical controls for standalone operation.
- Future architectural cleanup while preserving the verified legacy implementation.

## Prior Work / Acknowledgements

Thanks to [kjy00302/eo-ic100_radio](https://github.com/kjy00302/eo-ic100_radio) for an important reference during the initial investigation of Samsung EO-IC100 FM functionality and the Samsung protocol. That project presents a Python proof of concept for controlling EO-IC100's FM radio.

Here, Samsung's Android FM service is the host/controller and the RP2040 + SI4703 adapter provides the device-side FM hardware. This acknowledgement does not assert that the projects are identical or that source code was copied.

## Contributions

Reports and contributions on protocol observations, station validation, device compatibility and hardware documentation are welcome. Include board/core/library versions, phone/OS/app versions, reproduction steps and expected versus observed behavior. Remove account, network and device identifiers from logs and screenshots. Explain changes relative to the preserved hardware-verified baseline.

This is an unofficial interoperability project, with no affiliation with or endorsement from Samsung or Silicon Labs.

## License

This repository's own code is provided under the [MIT License](LICENSE). Copyright (c) 2026 sammik9660.

External dependencies, including Arduino-Pico, Adafruit TinyUSB and PU2CLR SI470X, retain their respective licenses. This project's license does not replace them.
