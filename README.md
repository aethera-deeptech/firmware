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

The repository has no device code yet. A device folder is added when its firmware is ready to build, and this section then says how to build and flash it.

## Configuration

No setting exists yet. The settings of each device are listed here when its folder is added.

## Documentation

- [Firmware guide of the backend](https://github.com/aethera-deeptech/backend/blob/main/docs/firmware-guide.md) is the contract that the hub follows.
- [CONTRIBUTING.md](CONTRIBUTING.md) explains how to make a change.
- [docs/ci-cd.md](docs/ci-cd.md) lists every workflow and every required check.
- [SECURITY.md](SECURITY.md) says how to report a vulnerability.

## Project layout

- `.github/` holds the workflows, the issue forms and the pull request template.
- `docs/` holds the project documentation.

## Contributing and security

Read [CONTRIBUTING.md](CONTRIBUTING.md) before you open a pull request, and everyone who takes part follows the [Code of Conduct](CODE_OF_CONDUCT.md). Report security problems privately as described in [SECURITY.md](SECURITY.md).

## License

GPL-3.0, see [LICENSE](LICENSE).
