# TODO

Open roadmap. Test reports from real tags are welcome.

## Verify on real hardware

Checked so far: DFU from stock 3.31.1, pairing and live values with a HomePod mini on one tag.

- [ ] Measure the current draw (e.g. Nordic PPK2) and replace the estimates in README
      "Battery and range options". Compare with stock 3.31.1 on the same tag.
- [ ] Battery drain over a few weeks across several tags.
- [ ] Range: reliable updates at about 10 m through walls with the default +4 dBm / 1022.5 ms.
- [ ] Event behaviour: a 1 °C change shows up quickly, small changes within about 5 minutes,
      the low battery notification fires.
- [ ] Firmware update over the HomeKit firmware (B + R) keeps the pairing.
- [ ] 15 s button hold removes the pairings and the tag can be paired again.
- [ ] Going back to stock firmware works as described in README "Flash".
- [ ] Home hub restart, and the tag out of range for a while, then back: it recovers without
      re-pairing.
- [ ] NFC shows temperature, humidity and battery.

## Possible improvements

- [ ] Debug build with HomeKit ADK and Ruuvi logs over SEGGER RTT, for use with an SWD probe
      (Ruuvi DevKit or J-Link). Logging is compiled out of the normal build.
- [ ] Per-tag setup codes: a build option or provisioning step, so a QR code matches only one
      tag. Today every tag built from the same `homekit-setup.json` shares the code.
- [ ] Revisit BLE connection parameters (slave latency) once current draw is measured. Skipped
      for now: the ADK drops idle links after about 10 s, and latency would slow HAP round trips.
- [ ] Tune the event thresholds and the 5 minute limit after real-world use.
- [ ] GitHub Actions workflow that runs the build, `make -C tests/host` and the simulator
      end-to-end test.
