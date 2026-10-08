# Changelog

All notable changes to the LazyDragon Oculus-8 analog IR sensor array are
documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Licensing: hardware under CC BY-NC-SA 4.0 (`LICENSE`), firmware under
  PolyForm Noncommercial 1.0.0 (`LICENSE-CODE`).
- `COMMERCIAL-LICENSE.md` for paid commercial/royalty licensing.
- README license badges, documentation index, and no-warranty notice.
- Repository structure aligned to the LazyDragon standard (`assets/`,
  `datasheets/`, `fabrication/`, `code/`; docs at root).

## [1.0.0] - 2026-10-07

### Added
- `oculus_8.ino`: ISR-driven 8-channel ADC reader (non-blocking, interrupt-based scan).
- Per-channel auto-calibration to 0..1000.
- Integer line position scaled x1000 (`-7000..+7000`).
- Sensor pattern classification: `ON_LINE`, `EDGE_LEFT`, `EDGE_RIGHT`, `JUNCTION`, `LOST`.
- Saturation detection and `lost`/`junction` counters over Serial at 115200 baud.
- Hardware: 8x QRE1113GR analog IR sensor array board.
- Documentation: `README.md`, `MANUAL.md`, `DEBUG.md`.
