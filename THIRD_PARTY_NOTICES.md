# Licensing scope and third-party notices

The root MIT license covers this project's original contributions. It does not
relicense upstream code or override its terms. The firmware is a mixed-license
work; do not describe the complete distribution as unrestricted MIT software.

## DutchMaker / Supervision-LCD-v2

Copyright (C) 2025 Ruud van Falier, DutchMaker.nl.

- Reference: https://github.com/DutchMaker/Supervision-LCD-v2
- Upstream license: https://github.com/DutchMaker/Supervision-LCD-v2/blob/main/LICENSE
- Included full text: `licenses/DutchMaker-NON-COMMERCIAL.txt`.
- Derived capture implementations: `firmware/src/main_proven.c` (including the
  accepted fast-field-handoff target) and `firmware/src/main_reference_exact.c`
  (historical diagnostic target).

The project's USB transport, synchronization qualification, queue/handoff,
Windows viewer and tests extend/adapt the reference work. Original additions
are MIT, but combining them with the derived capture code does not remove the
upstream non-commercial restriction. Keep the upstream attribution and license
with source and firmware binary distributions. Commercial use of the derived
firmware requires permission beyond the license shipped here.

The grayscale reconstruction was informed by DutchMaker's three-field
description; our host implementation is not a copy of its TFT renderer.

## xrip / watara-supervision-lcd

Reference: https://github.com/xrip/watara-supervision-lcd

Acknowledged as an earlier RP2040 LCD-bus capture reference. No upstream xrip
repository files or display drivers are bundled here. Its README's license
section is a placeholder; it is not represented as an MIT licensing grant.

## Raspberry Pi Pico SDK

Copyright 2020 (c) 2020 Raspberry Pi (Trading) Ltd.

The accepted firmware links against Pico SDK 1.5.1. Full root BSD-3-Clause
notice is included as `licenses/Pico-SDK-BSD-3-Clause.txt`. The SDK is an external
build dependency and is not vendored in this repository.

Reference: https://github.com/raspberrypi/pico-sdk/tree/1.5.1

The SDK's stdio implementation also includes an MIT notice for
Copyright (c) 2014 Marco Paland, reproduced in `licenses/Pico-stdio-MIT.txt`.

## TinyUSB

Copyright (c) 2018, hathach (tinyusb.org).

TinyUSB is supplied by the Pico SDK and linked into the firmware. Full MIT
notice is included as `licenses/TinyUSB-MIT.txt`.

Reference: https://github.com/hathach/tinyusb

Build dependencies, toolchains and optional historical targets may carry
additional upstream terms. These notices do not license unrelated third-party
files or game content. No ROMs, game recordings, user screenshots, account
credentials, or private diagnostic logs are included in this release.
