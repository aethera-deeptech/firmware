# WATTLINK firmware

[![Config lint](https://github.com/aethera-deeptech/firmware/actions/workflows/config.yaml/badge.svg)](https://github.com/aethera-deeptech/firmware/actions/workflows/config.yaml)
[![Security](https://github.com/aethera-deeptech/firmware/actions/workflows/security.yaml/badge.svg)](https://github.com/aethera-deeptech/firmware/actions/workflows/security.yaml)
[![License: GPL-3.0](https://img.shields.io/github/license/aethera-deeptech/firmware)](LICENSE)
[![Last commit](https://img.shields.io/github/last-commit/aethera-deeptech/firmware)](https://github.com/aethera-deeptech/firmware/commits/main)
[![Conventional Commits](https://img.shields.io/badge/Conventional%20Commits-1.0.0-fe5196?logo=conventionalcommits&logoColor=white)](https://www.conventionalcommits.org/)
[![Renovate](https://img.shields.io/badge/renovate-enabled-brightgreen?logo=renovatebot)](https://docs.renovatebot.com/)
[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/aethera-deeptech/firmware/badge)](https://scorecard.dev/viewer/?uri=github.com/aethera-deeptech/firmware)

Firmware for the devices of WATTLINK, an energy audit dashboard for small businesses in Indonesia. A plug or a current clamp measures power and sends it to a hub. The hub sends the readings to the [backend](https://github.com/aethera-deeptech/backend) over HTTPS.

## Devices

| Device | Job |
| :--- | :--- |
| Hub (gateway) | Collects the readings of the nodes over ESP-NOW and sends them to the backend |
| Plug | Measures one appliance and switches it with a relay |
| Clamp | Measures one machine with a split-core current transformer |

## Stack

- PlatformIO for building and flashing.
- ESP-NOW between the nodes and the hub, and HTTPS and JSON between the hub and the backend.
- GitHub Actions for every check, so a pull request needs no local build.

## Quick start

The Hub is the only device with firmware so far, in `gateway/`, and the code that it will share with the nodes is in `common/`. It is a PlatformIO project for the ESP32 DevKit V1 with the Arduino framework, on the espressif32 platform 6.13.0, which ships Arduino-ESP32 2.0.17. CI builds it and runs the host side tests on every pull request, so a board is only needed to see a change work on hardware. To build it yourself, install PlatformIO and run `pio run --project-dir gateway --environment hub`. The tests run with `pio test --project-dir gateway --environment native` and `pio test --project-dir common --environment native`. The second one links OpenSSL, so it needs the OpenSSL development headers on the machine. The Plug and the Clamp get their folders when their firmware is ready.

## Configuration

The pins and the version are constants in `gateway/src/config.h`. The only setting that the owner can change is the Wi-Fi of the building, which the Hub gets through a captive portal. The owner holds the button of the Hub for 3 seconds, joins the setup network that the display names, uses the code that it shows without the hyphens as the password, and fills in a page. The Hub joins only Wi-Fi networks on 2.4 GHz, because the ESP32 has no 5 GHz radio, and the setup page says so. No password is stored in the repository.

## Documentation

- [Firmware guide of the backend](https://github.com/aethera-deeptech/backend/blob/main/docs/firmware-guide.md) is the contract that the hub follows.
- [docs/espnow-protocol.md](docs/espnow-protocol.md) describes the frames between the nodes and the hub.
- [CONTRIBUTING.md](CONTRIBUTING.md) explains how to make a change.
- [docs/ci-cd.md](docs/ci-cd.md) lists every workflow and every required check.
- [docs/hub-flashing.md](docs/hub-flashing.md) says how to put a CI build on a hub board without installing PlatformIO.
- [SECURITY.md](SECURITY.md) says how to report a vulnerability.

## Project layout

- `common/` holds the code that the Hub and the nodes share, which is the frame codec, the payload of every frame type, the sealing and opening of the encrypted frames, the nonce and the key derivation. It has host side tests and no hardware code.
- `gateway/` holds the firmware of the Hub. The code that does not touch the hardware sits in `gateway/lib/` and is tested on the host.
- `.github/` holds the workflows, the issue forms and the pull request template.
- `docs/` holds the project documentation.

## Contributing and security

Read [CONTRIBUTING.md](CONTRIBUTING.md) before you open a pull request, and everyone who takes part follows the [Code of Conduct](CODE_OF_CONDUCT.md). Report security problems privately as described in [SECURITY.md](SECURITY.md).

## License

GPL-3.0, see [LICENSE](LICENSE).
