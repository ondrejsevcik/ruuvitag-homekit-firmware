<img src="assets/icon.svg" width="64" height="64" alt="">

# RuuviTag native HomeKit firmware

Custom firmware that makes a RuuviTag a **native Apple HomeKit accessory** over Bluetooth LE.
No bridge, Homebridge or extra app: pair it in the Home app with a setup code. A HomePod or
Apple TV acts as the home hub for remote access and automations.

It is a fork of the official [Ruuvi firmware v3.31.1](https://github.com/ruuvi/ruuvi.firmware.c)
(see [README.ruuvi.md](README.ruuvi.md)) with a new `homekit` build variant, built on Apple's
open source [HomeKit ADK](https://github.com/apple/HomeKitADK) (non-commercial use).

| HomeKit service | Characteristics |
| --- | --- |
| Temperature Sensor | Current temperature (-40…85 °C, 0.1 °C) |
| Humidity Sensor | Relative humidity (%) |
| Battery | Level (CR2477, 2.5–3.0 V), low battery (Ruuvi Station thresholds), not chargeable |

Air pressure and acceleration are not exposed: HomeKit has no standard type for them.

## What changes compared to the stock firmware

- Bluetooth belongs to HomeKit. The tag **no longer sends Ruuvi broadcasts**, so Ruuvi Station
  and Ruuvi Gateway stop seeing it. NFC still works and shows the current readings.
- Sensors are sampled every 10 s. Changes (≥ 0.2 °C, ≥ 1 % RH) are pushed to the home hub as
  encrypted HomeKit broadcasts, so the hub does not need to keep a connection open.
- History logging, the Ruuvi GATT services (log download, configuration) and the remote
  "enter bootloader" (buttonless DFU) service are removed. Entering update mode needs the button.
- Flash storage is limited to the 20 kB the Ruuvi bootloader preserves during updates, so
  HomeKit pairings survive firmware updates.
- The Bluetooth MAC address changes (HomeKit uses its own random static address).

### Button

| Hold | Action |
| --- | --- |
| 5–15 s, then release | Enter the bootloader for a firmware update (pairings are kept) |
| 15 s | Remove all HomeKit pairings, the tag becomes pairable again |

## Build

Requirements: `arm-none-eabi-gcc`, `make`, `python3` with `cryptography`, `curl`, `unzip`.
Optional: `qrencode` to render the setup QR code.

```sh
git submodule update --init --recursive
tools/build_homekit.sh
```

The script downloads the nRF5 SDK 15.3 and Ruuvi's public "open" signing key (both checksum
pinned), builds the firmware and writes to `dist/`:

- `ruuvitag_b_homekit_<version>_dfu_app.zip` – update package for the stock Ruuvi bootloader
- `ruuvitag_b_homekit_<version>_app.hex` – application image for SWD flashing

On the first build it creates `homekit-setup.json` with a random **setup code** and prints it
(plus a QR code and `homekit-setup-qr.svg` if `qrencode` is installed). Keep the file: later
builds reuse the code.

To choose your own code, run `tools/homekit_provision.py show --new --code 123-45-670`, then
rebuild and flash again. The code is baked into the firmware, so a tag running an older build
still expects the old code.

**Treat `homekit-setup.json` and everything in `dist/` as secret.** The firmware contains only
the SRP salt and verifier, not the code itself. An 8-digit code can still be brute-forced offline
from them, so anyone with the image can recover the code and pair with the tag whenever it is
unpaired.

## Flash

1. Install **nRF Connect** or **nRF Device Firmware Update** (Nordic) on the phone and copy the
   `_dfu_app.zip` to it.
2. Put the tag into bootloader mode:
   - stock Ruuvi firmware: hold the button. After 5 s it erases its history log and restarts
     into the bootloader while you are still holding;
   - HomeKit firmware: hold the button 5–15 s and release (pairings are kept);
   - either firmware: hold the button while inserting the battery or pressing reset.
3. Connect to the device advertising as `RuuviBoot` and start the update with the zip.

To go back to stock, first remove the accessory in the Home app, or hold the button 15 s.
Pairing data sits in flash that firmware updates preserve. If you skip this, Home keeps an
unreachable accessory, and flashing the HomeKit firmware again restores the old pairing.
Then flash an official `ruuvitag_b_armgcc_ruuvifw_default_*_dfu_app.zip` the same way.

## Pair

Home app → **+** → Add Accessory → scan the QR code or enter the setup code. iOS warns that the
accessory is not certified; choose *Add Anyway*.

## Tests

Requirements, in addition to the build: run `tools/build_homekit.sh` once (it downloads the
nRF5 SDK); a 32-bit ARM Linux toolchain and runtime (Debian / Raspberry Pi OS:
`apt install gcc-arm-linux-gnueabihf libc6:armhf`); an AArch64 machine with 32-bit userland
support, such as a Raspberry Pi 4 (elsewhere, qemu user-mode emulation via `RUN=...` should work but is untested).

```sh
make -C tests/host          # crypto (Apple ADK vectors on nrf_oberon), provisioning, simulator build
python3 -m venv /tmp/hkvenv && /tmp/hkvenv/bin/pip install aiohomekit==4.0.1
/tmp/hkvenv/bin/python tests/sim/hap_e2e.py     # HK_MTU=23 for minimum ATT MTU
```

The end-to-end test uses aiohomekit internals, so other versions may need adjustments. It also
works around an aiohomekit bug: aiohomekit hashes the accessory's SRP public key without
padding, so about 1 pairing in 256 fails. iOS is not affected.

`tests/host` builds for 32-bit ARM Linux so that it links the same prebuilt crypto library as
the tag (runs natively on a Raspberry Pi). `tests/sim` runs the firmware's HomeKit code against a
simulated SoftDevice and flash, driven by [aiohomekit](https://github.com/Jc2k/aiohomekit), the
HomeKit controller used by Home Assistant: discovery, pair setup, pair verify, reads, connected
and broadcast events, identify, long writes, pairing removal.

## Layout

| Path | Contents |
| --- | --- |
| `src/homekit/` | HomeKit accessory, attribute database, platform layer (BLE on S132, FDS key-value store, timers, Oberon crypto) |
| `src/application_config/application_mode_homekit.h` | Variant configuration |
| `src/app_comms_homekit.c` | Communication setup of the variant |
| `src/targets/ruuvitag_b/armgcc/ruuvitag_b_homekit.ld` | Memory layout (more SoftDevice RAM, 20 kB storage) |
| `tools/` | Build script, setup code provisioning, DFU packaging |
| `tests/` | Host tests and simulator |
| `external/HomeKitADK` | Apple HomeKit ADK (submodule) |
