# TODO

## Before publishing

- [ ] Rewrite the commit author. The commits use a placeholder identity
      (`Ruuvi HomeKit Dev <ruuvi-homekit@localhost>`), which GitHub won't link to an account and
      which reads like an official Ruuvi project. For example, with your GitHub noreply address:
      `git rebase --root --exec 'git commit --amend --no-edit --reset-author'` after setting
      `user.name` / `user.email`.
- [ ] Pick a repository name that doesn't look official, e.g. `ruuvitag-homekit-firmware`.
- [ ] Push only the `homekit` branch (`git push -u origin homekit`, optionally as `main`). Don't
      use `git push --mirror` or `--all`: the local `refs/t3/*` checkpoint refs are tool state.
- [ ] Never commit `homekit-setup.json`, `homekit-setup-qr.svg` or `dist/` (already in
      `.gitignore`). Release builds made from your setup file would let anyone pair your
      unpaired tags, so publish source only, or attach builds made with a throwaway setup code.
- [ ] Optional: add a GitHub Actions workflow that runs the build, `make -C tests/host` and the
      simulator end-to-end test.

## Verify on real hardware

Checked so far (2026-10-25): DFU from stock 3.31.1, pairing and live values with a HomePod mini
on one tag.

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
